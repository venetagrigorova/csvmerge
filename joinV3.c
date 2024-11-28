#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


// Put all implemented performance optimizations here (relative to base.c)
// Currently implemented in this file
// - Threeway merge join implemented
// - created utility functions _copyField, _mergeTempTables
// - Remove numFields from Record struct, thus mostly passed as input parameter
// - Used INT128 encoding, major overhauls to low-level functions
// -
// -
// -

#define FIELD_LENGTH 23  // 22 chars + string terminator
#define CHUNK_SIZE 1000000    // Number of records per chunk
#define BUFFER_SIZE 128      // Read/write buffer size
#define MAX_NUM_CHUNKS 30  // Should have CHUNK_SIZE * MAX_NUM_CHUNKS >= 12 Mio.
#define MAX_PRINT_LINES 25 // Only for testing, limit the number of lines printed out at once
#define MAX_NUM_CHARACTERS 55 // 1 + 10 + 26 + 17 + 1 (can handle \0, 0-9, A-Z, a-q, and one unknown character)
#define INT_128 unsigned __int128


// Structure for record with no fixed number of fields
typedef struct {
    INT_128* fields;  // fields is an array of unsigned int128, each representing one field
} Record;

// Structure for managing chunks fully loaded into memory
// Has an array of records, and number of records and fields of each record
typedef struct {
    Record* records;
    int numRecords;
    int numFields;
} Chunk;

// Stucture for managing table
// A table is never loaded into memory - it only has one record as well as information on where to get the next record
typedef struct {
    FILE* file;
    Record currentRecord;
    bool endOfFile;
    bool file_is_encoded; // whether the file contains strings or 128bit integers
    int numFields;
} Table;

// Buffer structure to hold a varying number of records
// Has key field
// Used internally during join of tables
typedef struct {
    INT_128 key;
    Record* records;
    int numRecords;
    int numFields;
} JoinBuffer;



// -----------= CONSTUCTORS AND DESTRUCTORS =----------------

// Initialize a record with given number of fields at a certain adress
void initRecord(Record* record, const int numFields) {
    record->fields = (INT_128*)malloc(numFields * sizeof(INT_128));
    if(!record->fields) {
        perror("Failed to allocate memory for record");
        exit(EXIT_FAILURE);
    }
    // Initialize all fields to be empty
    for (int i = 0; i < numFields; i++) {
        record->fields[i] = 0;
    }
}


// Free memory allocated for a record
void freeRecord(Record* record) {
    free(record->fields);
    if(record) record = NULL;
}

void _copyField(const Record* dest, const Record* source, const int dest_index, const int source_index) {
    dest->fields[dest_index] = source->fields[source_index];
}

// Copy to destination record from source record, assuming they have the same number of fields
void copyRecord(const Record* dest, const Record* source, const int numFields) {
    // Copy all fields
    for (int i = 0; i < numFields; i++) {
        _copyField(dest, source, i, i);
    }
}


// Create and initialize a chunk
Chunk* createChunk(const int numRecords, const int numFields) {
    Chunk* chunk = (Chunk*)malloc(sizeof(Chunk));
    Record* records = (Record*)malloc(numRecords * sizeof(Record));
    if (!chunk | !records) {
        perror("Failed to allocate memory for chunk");
        exit(EXIT_FAILURE);
    }
    for (int i = 0; i < numRecords; i++) {
        initRecord(&records[i], numFields);
    }
    chunk->records = records;
    chunk->numRecords = 0;
    chunk->numFields = numFields;
    return chunk;
}


void freeChunk(Chunk* chunk) {
    for (int i = 0; i < chunk->numRecords; i++) {
        freeRecord(&chunk->records[i]);
    }
    free(chunk->records);
    if(chunk) {
        free(chunk);
        chunk = NULL;
    }
}


// Create and initialize a table
Table* createTable(const int numFields) {
    Table* table = malloc(sizeof(Table));
    if(!table) {
        perror("Failed to allocate memory for table");
        exit(EXIT_FAILURE);
    };
    table->numFields = numFields;
    table->endOfFile = 0;
    table->file_is_encoded = false;

    Record* currentRecord = (Record*)malloc(sizeof(Record));
    initRecord(currentRecord, numFields);
    table->currentRecord = *currentRecord;
    return table;
}


// Free memory allocated for a table
void freeTable(Table* table) {
    freeRecord(&table->currentRecord);
    if(table->file)    fclose(table->file);
    if(table) {
        free(table);
        table = NULL;
    }
}



// -----------= ALL FUNCTIONS CONCERNING THE JOIN BUFFER =----------------

JoinBuffer* createJoinBuffer(const int numFields) {
    const int numRecords = BUFFER_SIZE;
    JoinBuffer* buffer = (JoinBuffer*)malloc(sizeof(JoinBuffer));
    Record* records = (Record*)malloc(numRecords * sizeof(Record));
    if (!buffer | !records) {
        perror("Failed to allocate memory for buffer");
        exit(EXIT_FAILURE);
    }
    for (int i = 0; i < BUFFER_SIZE; i++) {
        initRecord(&records[i], numFields);
    }
    buffer->key = 0;
    buffer->records = records;
    buffer->numRecords = 0;
    buffer->numFields = numFields;
    return buffer;
}


bool recordFitsBuffer(const Record* record, const int key_index, const JoinBuffer* buffer) {
    if(buffer->numRecords == 0) {
        return false;
    }
    int compare = (buffer->key == record->fields[key_index]);
    return compare;
}


void writeRecordToBuffer(Record* record, JoinBuffer* buffer, const int right_on) {
    if(buffer->numRecords == 0) {
        buffer->key = record->fields[right_on];
    }
    else if(buffer->numRecords == BUFFER_SIZE) {
        perror("Join buffer overflow - increase BUFFER_SIZE");
        exit(EXIT_FAILURE);
    }
    else if(!recordFitsBuffer(record, right_on, buffer)) {
        perror("Tried to write record to incompatible buffer");
        exit(EXIT_FAILURE);
    }
    copyRecord(&buffer->records[buffer->numRecords], record, buffer->numFields);
    buffer->numRecords++;
}


void emptyBuffer(JoinBuffer* buffer) {
    buffer->key = 0;
    for (int i = 0; i < buffer->numRecords; i++) {
        initRecord(&buffer->records[i], buffer->numFields);
    }
    buffer->numRecords = 0;
}


void freeJoinBuffer(JoinBuffer* buffer) {
    for (int i = 0; i < BUFFER_SIZE; i++) {
        freeRecord(&buffer->records[i]);
    }
    free(buffer->records);
    if(buffer) {
        free(buffer);
        buffer = NULL;
    }
}



// -----------= INPUT FROM FILE INTO TABLE/CHUNK =----------------

INT_128 encodeField(const char* field) {
    INT_128 result = 0;
    INT_128 offset = 1;
    char character;
    unsigned int add = 0;
    for (int i = 0; i < FIELD_LENGTH & field[i] != ',' & field[i] != '\n' ; i++) {
        character = field[i];
        if (character >= 'A' && character <= 'Z') {
            add = character - 'A' + 1;  // 'A' -> 1, 'B' -> 2, ..., 'Z' -> 26
        } else if (character >= '0' && character <= '9') {
            add = character - '0' + 27; // '0' -> 27, '1' -> 28, ..., '9' -> 36
        } else if (character >= 'a' && character <= 'q') {
            add = character - 'a' + 37; // 'a' -> 37, ..., 'q' -> 53
        } else if (character == '\0') {
            add = 54;           // '\0' -> 54
        } else add = 0;  // any unknown characters -> 0
        result = result + add * offset;
        offset = offset * MAX_NUM_CHARACTERS;
    }
    return result;
}

// decodes Int128 into output string and returns the length (not including terminating null but
// possibly including custom null character in string)
int decodeField(INT_128 code, char* output) {
    INT_128 value = code;
    unsigned int remainder = 0;
    int len = 0;
    for(len = 0; len < FIELD_LENGTH; len++) {
        remainder = value % MAX_NUM_CHARACTERS;
        if (remainder >= 1 && remainder <= 26) {
            output[len] =  'A' + (remainder - 1);  // 1 -> 'A', 2 -> 'B', ..., 26 -> 'Z'
        } else if (remainder >= 27 && remainder <= 36) {
            output[len] =  '0' + (remainder - 27); // 27 -> '0', ..., 36 -> '9'
        } else if (remainder >= 37 && remainder <= 53) {
            output[len] = 'a' + (remainder - 37); // 37 -> 'a', ..., 53 -> 'q'
        } else if (remainder == 54) {
            output[len] = '\0';             // 54 -> '\0'
        } else output[len] = '?';
        value = value / MAX_NUM_CHARACTERS;
        // breakout condition
        if(value == 0) {
            output[len+1] = '\0';
            len++;
            break;
        }
    }
    return len;
}

void sanitizeLine(char *line, const int numFields) {
    // Unnesscessary due to encoding
    return;
}


void loadLineIntoRecord(const char* line, const Record* record, const int numFields) {
    int field_start = 0;
    int current_field = 0;
    for(int i = 0; i < numFields * FIELD_LENGTH & line[i] != '\n'; i++) {
        // Found end of current field
        if(line[i] == ',') {
            record->fields[current_field] = encodeField(&line[field_start]);
            field_start = i+1;
            current_field++;
        }
    }
    // Encode last field
    record->fields[current_field] = encodeField(&line[field_start]);
}


// Used in Table: Read a single record to replace the current record
void readNextRecord(Table* table) {
    if(table->endOfFile == true) {
        perror("No more records in file");
        exit(EXIT_FAILURE);
    }
    if(table->file_is_encoded) {
        size_t read = fread(table->currentRecord.fields, sizeof(INT_128), table->numFields, table->file);
        if(feof(table->file)) {
            table->endOfFile = true;
        }
        else if (read != table->numFields) {
            fprintf(stderr, "Error: Failed to read all integers from file.\n");
        }
        return;
    }

    // Else if table->file is not encoded
    const int length = table->numFields*FIELD_LENGTH + 1; // terminating character
    char line[length];

    if(fgets(line, length, table->file) != NULL ) {
        loadLineIntoRecord(line, &table->currentRecord, table->numFields);
    }
    else { // End of file reached
        table->endOfFile = true;

        // This last section is for debugging and should be removable at the very end of the project
        // It replaces the last currentRecord with the message THE END in every field
        char *errormsg = (char *)malloc(table->numFields * FIELD_LENGTH + 1); // +1 for null terminator

        if (errormsg == NULL) {
            perror("Memory allocation failed at last line");
            exit(EXIT_FAILURE);
        }
        errormsg[0] = '\0';
        for (int i = 0; i < table->numFields; i++) {
            strcat(errormsg, "THEEND");
            if (i < table->numFields - 1) {
                strcat(errormsg, ",");
            }
        }
        strcat(errormsg, "\n\0");
        loadLineIntoRecord(errormsg, &table->currentRecord, table->numFields);
        free(errormsg);
    }
}


// read from input file into chunk until chunk is full
Chunk* loadFileIntoChunk(FILE* inputFile, const int numFields, bool is_encoded) {
    Chunk* chunk = createChunk(CHUNK_SIZE, numFields);
    int row = 0;

    // If the file is encoded
    if(is_encoded) {
        for(row; row < CHUNK_SIZE; row++) {
            size_t read = fread(chunk->records[row].fields, sizeof(INT_128), numFields, inputFile);
            if(feof(inputFile)) {
                break;
            }
            if (read != numFields) {
                fprintf(stderr, "Error: Failed to read all integers from file.\n");
            }
        }
        chunk->numRecords = row;
        return chunk;
    }

    // Else if the file is not encoded
    const int length = numFields*FIELD_LENGTH + 1; // content + separators (, or \n) + terminator
    char line[length];

    for(row; row < CHUNK_SIZE; row++) {
        if(feof(inputFile)) {
            row--; // Since we counted one line too much
            break;
        }
        if(fgets(line, length, inputFile) != NULL ) {
            loadLineIntoRecord(line, &chunk->records[row], chunk->numFields);
        }
    }

    chunk->numRecords = row;
    return chunk;
}


Table* loadFileIntoTable(const char* inputFileName, const int numFields, bool file_is_encoded) {
    Table* table = createTable(numFields);
    table->file = fopen(inputFileName, "r");
    table->file_is_encoded = file_is_encoded;
    if(!table->file) {
        perror("Failed to open file");
        exit(EXIT_FAILURE);
    }
    return table;
}



// -----------= OUTPUT FUNCTIONS =----------------

// Write a single record to file
void writeRecordToFile(const Record* record, FILE* output_file, const int numFields, bool encode_file) {
    // Write encoded
    if(encode_file) {
        INT_128 value = 0;
        for(int i=0; i < numFields; i++) {
            value = record->fields[i];
            // write as raw binary
            fwrite(&value, sizeof(value), 1, output_file);
        }
        return;
    }

    // write as string
    char field[FIELD_LENGTH];
    int length = 0;
    for (int i = 0; i < numFields; i++) {
        length = decodeField(record->fields[i], field);
        fwrite(field, sizeof(char), length, output_file);
        if (i < numFields - 1) {
            fputc(44, output_file); // put a comma after the field
        }
    }
    fputc(012,output_file); // put a newline after the line
}

void _writeChunkToFileStream(const Chunk* chunk, FILE* file) {
    for (int i = 0; i < chunk->numRecords; i++) {
        writeRecordToFile(&chunk->records[i], file, chunk->numFields, true);
    }
}

// Write an entire chunk to file
void writeChunkToFile(const Chunk* chunk, const char* outputFilename) {
    FILE* file = fopen(outputFilename, "wb+");
    if (!file) {
        perror("Error opening file for writing");
        exit(EXIT_FAILURE);
    }
    _writeChunkToFileStream(chunk, file);
    fclose(file);
}

void _writeTableToFileStream(Table* table, FILE* file, bool encode_file) {
    readNextRecord(table);
    while(!feof(table->file)) {
        writeRecordToFile(&table->currentRecord, file, table->numFields, encode_file);
        readNextRecord(table);
    }
}

// Write an entire table to file
void writeTableToFile(Table* table, const char* outputFilename, bool encode_file) {
    FILE* file = fopen(outputFilename, "wb+");
    if (!file) {
        perror("Error opening file for writing");
        exit(EXIT_FAILURE);
    }
    _writeTableToFileStream(table, file, encode_file);
    fclose(file);
}



// -----------= SORTING FUNCTIONS =----------------

// Compare records based on left and right field
int compareRecordsOnFields(const void* left, const void* right, const int left_on, const int right_on) {
    const Record* r1 = (const Record*)left;
    const Record* r2 = (const Record*)right;
    if(r1->fields[left_on] < r2->fields[right_on])  return -1;
    if(r1->fields[left_on] > r2->fields[right_on])  return 1;
    return 0;
}

int compareRecordsOnZero(const void* a, const void* b) {
    return compareRecordsOnFields(a, b, 0, 0);
}

int compareRecordsOnThird(const void* a, const void* b) {
    return compareRecordsOnFields(a, b, 3, 3);
}


// Sort one chunk in-place in memory
void sortChunk(Chunk* chunk, int on_index) {
    if(on_index == 0) {
        qsort(chunk->records, chunk->numRecords, sizeof(Record), compareRecordsOnZero);
    }
    else if (on_index == 3) {
        qsort(chunk->records, chunk->numRecords, sizeof(Record), compareRecordsOnThird);
    }
    else {
        perror("Not implemented");
        exit(EXIT_FAILURE);
    }
    // TODO: Change this to a stable sorting algorithm that keeps equal elements in the same order
    // TODO: Add ability to sort on any index, not just the first or second
    // There has to be a better way to do this....
}


Table* ExternalMergeSort(const Table* table, const char* outputFileName, const int on_index) {
    int numChunks = 0;
    Table* tempTables[MAX_NUM_CHUNKS];

    // Process table as chunks
    // 1) Split off chunk from table
    // 2) Sort in-memory
    // 3) Write as temp. file to disc
    // 4) Remove chunk from memory
    while(!feof(table->file) && numChunks < MAX_NUM_CHUNKS) {
        // Create new chunk file
        char tempFilename[32];
        sprintf(tempFilename, "temp_%d.bin", numChunks);

        Chunk* chunk = loadFileIntoChunk(table->file, table->numFields, table->file_is_encoded);
        sortChunk(chunk, on_index);
        writeChunkToFile(chunk, tempFilename);
        freeChunk(chunk);

        tempTables[numChunks] = loadFileIntoTable(tempFilename, table->numFields, true);
        readNextRecord(tempTables[numChunks]);

        numChunks++;
    }

    _mergeTempTables(tempTables, outputFileName, numChunks, on_index);
    // Moved this loop into another function to use it in another part of the code
    /*
    while(true) {
        int numRemainingChunks = numChunks;
        ......
    }
    */
    char tempFilename[32];
    for(int i = 0; i < numChunks; i++) {
        freeTable(tempTables[i]);
        sprintf(tempFilename, "temp_%d.bin", i);
        if(remove(tempFilename)!=0) {
            perror("Error deleting file");
        };
    }

    Table* outputTable = loadFileIntoTable(outputFileName, table->numFields, true);
    return outputTable;
}

void _mergeTempTables(Table** tempTables, const char* outputFileName, const int numChunks, const int on_index) {
    FILE* output = fopen(outputFileName, "wb+");
    if(!output) {
        perror("Error opening output file for writing");
        exit(EXIT_FAILURE);
    }
    // Combine temp Tables
    // 1) Compare the current records of all active temp tables
    // 2) Find out which is the smallest
    // 3) Print that record into the output file
    // 4) Repeat until all records are processed
    while(true) {
        int numRemainingChunks = numChunks;
        int minRecordIndex = 0;
        if(tempTables[0]->endOfFile) numRemainingChunks--;

        for(int i = 1; i < numChunks; i++) {
            int difference = compareRecordsOnFields(&tempTables[i]->currentRecord,
                                            &tempTables[minRecordIndex]->currentRecord,
                                            on_index, on_index);
            // Replace if value is smaller and Table valid
            if((difference < 0) & (!tempTables[i]->endOfFile)) {
                minRecordIndex = i;
            }
            // Needed only if first temp file is no longer valid
            if((!tempTables[i]->endOfFile) & (tempTables[0]->endOfFile) & (minRecordIndex==0)) {
                minRecordIndex = i;
            }
            // Check if any tables are still valid
            if(tempTables[i]->endOfFile) {
                numRemainingChunks--;
            }
        }
        // Breakout condition
        if(numRemainingChunks == 0) {
            break;
        }
        writeRecordToFile(&(tempTables[minRecordIndex]->currentRecord), output, tempTables[0]->numFields, true);
        readNextRecord(tempTables[minRecordIndex]);
    }
    fclose(output);
}


//  -----------= JOIN FUNCTIONS =----------------

void _mergeTwoRecords(const Record* left, const Record* right,
                        const Record* target) {
    _copyField(target, left, 0, 3 ); // D
    _copyField(target, left, 1, 0); // A
    _copyField(target, left, 2, 1); // B
    _copyField(target, left, 3, 2); // C
    _copyField(target, right, 4, 1); // E
}

void _mergeThreeRecordsIntoRecord(const Record* left, const Record* middle, const Record* right,
                                  const Record* target)
{
    _copyField(target, left, 0, 0 ); // A
    _copyField(target, left, 1, 1);  // B
    _copyField(target, middle, 2, 1); // C
    _copyField(target, right, 3, 1);   //D
}

// Just a wrapper for _mergeThreeRecordsIntoRecord
void processThreeRecords(const Record* left, const Record* middle, const Record* right,
                          Chunk* outputChunk, int* index_of_chunk) {

    const int index_to_sort_on = 3; // Hardcoded to sort on field D

    // Whenever the chunk is full, sort it and write it out to file
    if(outputChunk->numRecords == CHUNK_SIZE) {
        char tempFilename[32];
        sprintf(tempFilename, "temp_join_%d.bin", *index_of_chunk);

        sortChunk(outputChunk, index_to_sort_on); // Sort chunk for next step
        writeChunkToFile(outputChunk, tempFilename); // Write out chunk to temp file
        outputChunk->numRecords = 0; // Pretend the chunk is empty again

        (*index_of_chunk)++; // increment index of chunk
    }

    // Take the first free record as outputRecord
    Record* outputRecord = &(outputChunk->records[outputChunk->numRecords]);

    if(compareRecordsOnFields(left, right, 0, 0) != 0 ||
        compareRecordsOnFields(left, middle, 0, 0) != 0) {
        perror("Tried to merge three incompatible records");
        exit(EXIT_FAILURE);
    }

    _mergeThreeRecordsIntoRecord(left, middle, right, outputRecord);
    outputChunk->numRecords++;
}


Table* joinTables(Table* left, Table* right, const char* outputFileName) {
    FILE* output = fopen(outputFileName, "wb+");
    if(!output) {
        perror("Error opening output file for writing");
        exit(EXIT_FAILURE);
    }

    const int numFieldsOutput = 5;
    JoinBuffer* buffer = createJoinBuffer(right->numFields);
    Record* tempRecord = malloc(sizeof(Record));
    initRecord(tempRecord, numFieldsOutput);

    bool is_in_join_phase = false;

    readNextRecord(left);
    readNextRecord(right);
    while(!left->endOfFile) {
        int comparison = compareRecordsOnFields(&left->currentRecord, &right->currentRecord, 3, 0);
        if(right->endOfFile) {
            comparison = -1;
        }

        if(comparison == 0) {
            is_in_join_phase = true;
            // Load right record into buffer
            writeRecordToBuffer(&right->currentRecord, buffer, 0);

            // Write the found joined records to output
            _mergeTwoRecords(&left->currentRecord, &right->currentRecord, tempRecord);
            writeRecordToFile(tempRecord, output, numFieldsOutput, true);

            // Advance right table
            readNextRecord(right);
        }
        else if (comparison < 0) {
            readNextRecord(left);
            if(!is_in_join_phase | left->endOfFile) {
                continue;
            }

            if(recordFitsBuffer(&left->currentRecord, 3, buffer)) {
                // In this case we can match the left record to all previous right records that were loaded
                // into the buffer. For each of the Records in the buffer output a join result of the current left
                // row with one previous right row.
                for(int i = 0; i < buffer->numRecords; i++) {

                    _mergeTwoRecords(&left->currentRecord,&buffer->records[i], tempRecord);
                    writeRecordToFile(tempRecord, output, numFieldsOutput, true);
                }
            }
            else if(buffer->numRecords != 0) {
                // In this case we don't need the buffer anymore
                emptyBuffer(buffer);
                is_in_join_phase = false; //end of current join phase
            }
        }
        else {
            readNextRecord(right);
        }
    }
    fclose(output);
    freeJoinBuffer(buffer);

    Table* result = loadFileIntoTable(outputFileName, numFieldsOutput, true);
    return result;
}


Table* joinThreeTables(Table* left, Table* middle, Table* right,
                        const char* outputFileName) {
    JoinBuffer* middleBuffer = createJoinBuffer(middle->numFields);
    JoinBuffer* rightBuffer = createJoinBuffer(right->numFields);

    const int numFieldsResult = 4; // Subtract the one common join field twice
    Chunk* outputChunk = createChunk(CHUNK_SIZE, numFieldsResult);
    int numChunks = 0;
    const int index_to_sort_on = 3;

    readNextRecord(left);
    readNextRecord(middle);
    readNextRecord(right);
    // Comparison values take -1 (or negative) if false, 0 if equal, 1 (or positive) if true
    int l_greater_r = compareRecordsOnFields(&left->currentRecord, &right->currentRecord, 0, 0);
    int l_greater_m = compareRecordsOnFields(&left->currentRecord, &middle->currentRecord, 0, 0);
    int m_greater_r = compareRecordsOnFields(&middle->currentRecord, &right->currentRecord, 0, 0);

    bool is_in_join_phase = false;
    bool middle_active = true, right_active = true;

    // We iterate while there are still records in the left table
    // since even if the middle and right table are finished, there could still be joinable records in the buffers
    while( !left->endOfFile ) {
        if(l_greater_m == 0 && l_greater_r == 0) {  // case (B B B), all active
            // enter a join phase - from here on out we need buffers
            is_in_join_phase = true;

            // Load right record into buffer
            writeRecordToBuffer(&right->currentRecord, rightBuffer, 0);

            // Write the found joined records to output
            processThreeRecords(&left->currentRecord,
                              &middle->currentRecord,
                              &right->currentRecord,
                              outputChunk, &numChunks);

            // Advance right
            readNextRecord(right);
            m_greater_r = compareRecordsOnFields(&middle->currentRecord, &right->currentRecord, 0, 0);
            l_greater_r = compareRecordsOnFields(&left->currentRecord, &right->currentRecord, 0, 0);
            if(right->endOfFile) {
                right_active = false;
                l_greater_r = -1;
                if(middle_active)   m_greater_r = -1;
                else                m_greater_r = 0;
            }
        }
        else if (l_greater_m == 0 & l_greater_r < 0) { // case (B B C) or (B B END)
            // Only buffer if we are in active join phase
            if (is_in_join_phase) {
                writeRecordToBuffer(&middle->currentRecord, middleBuffer, 0);
            }

            // Advance middle
            readNextRecord(middle);
            m_greater_r = compareRecordsOnFields(&middle->currentRecord, &right->currentRecord, 0, 0);
            l_greater_m = compareRecordsOnFields(&left->currentRecord, &middle->currentRecord, 0, 0);
            if(middle->endOfFile) {
                middle_active = false;
                l_greater_m = -1;
                if(right_active)   m_greater_r = 1;
                else               m_greater_r = 0;
            }

            // skip rest if not in join phase or we finished middle
            if (!is_in_join_phase | !middle_active) {
                continue;
            }

            // Merge phase of first order
            if(recordFitsBuffer(&middle->currentRecord, 0, rightBuffer)) {
                // In this case we can match the left, middle records to all previous right records that were loaded
                // into the buffer. For each of the Records in the buffer output a join result of the current left
                // row and middle row with one previous right row.
                for(int i = 0; i < rightBuffer->numRecords; i++) {
                    processThreeRecords(&left->currentRecord,
                                                &middle->currentRecord,
                                                &rightBuffer->records[i],
                                                outputChunk, &numChunks);
                }
            }
        }
        else if (l_greater_m < 0 & l_greater_r < 0) {  // case (B C D) or (B C C) or (B D C) or (B C B)
            readNextRecord(left);
            l_greater_m = compareRecordsOnFields(&left->currentRecord, &middle->currentRecord, 0, 0);
            l_greater_r = compareRecordsOnFields(&left->currentRecord, &right->currentRecord, 0, 0);
            if(left->endOfFile) {
                continue;
            }
            if(!middle_active)  l_greater_m = -1;
            if(!right_active)   l_greater_r = -1;

            // guard clause: if we are not in a join phase then we can just read the next row in the left
            if (!is_in_join_phase) {
                continue;
            }

            // If we are in a join phase we need to do more work
            if(recordFitsBuffer(&left->currentRecord, 0, middleBuffer)) {
                for(int i = 0; i < middleBuffer->numRecords; i++) {
                    for(int j = 0; j < rightBuffer->numRecords; j++) {
                        processThreeRecords(&left->currentRecord,
                                                   &middleBuffer->records[i],
                                                   &rightBuffer->records[j],
                                                   outputChunk, &numChunks);
                    }
                }
            }
            else { // Since the next left record did not fit the patter, this is the end of the current join phase
                is_in_join_phase = false;
                emptyBuffer(middleBuffer);
                emptyBuffer(rightBuffer);
            }
        }
        // Otherwise just advance the table with the smallest record
        else if (l_greater_m > 0 && m_greater_r < 0) { // Case (C B C) or (C B D) or (D B C) with possible E=End
            // Advance middle
            readNextRecord(middle);
            m_greater_r = compareRecordsOnFields(&middle->currentRecord, &right->currentRecord, 0, 0);
            l_greater_m = compareRecordsOnFields(&left->currentRecord, &middle->currentRecord, 0, 0);
            if(middle->endOfFile) {
                middle_active = false;
                l_greater_m = -1;
                if(right_active)   m_greater_r = 1;
                else               m_greater_r = 0;
            }
        }
        else  { // If none of the previous conditions were true then the right table must have one of the smallest records
            // Advance right
            readNextRecord(right);
            m_greater_r = compareRecordsOnFields(&middle->currentRecord, &right->currentRecord, 0, 0);
            l_greater_r = compareRecordsOnFields(&left->currentRecord, &right->currentRecord, 0, 0);
            if(right->endOfFile) {
                right_active = false;
                l_greater_r = -1;
                if(middle_active)   m_greater_r = -1;
                else                m_greater_r = 0;
            }
        }
    }
    // All records are correctly joined at this point

    // Write out the remaining chunk which is not yet full
    char tempFilename[32];
    sprintf(tempFilename, "temp_join_%d.bin", numChunks);
    if(outputChunk->numRecords > 0) {
        sortChunk(outputChunk, index_to_sort_on); // Sort chunk for next step
        writeChunkToFile(outputChunk, tempFilename); // Write out chunk to temp file
        outputChunk->numRecords = 0; // Pretend the chunk is empty again

        numChunks++; // increment index of chunk
    }

    // Cleanup
    freeJoinBuffer(rightBuffer);
    freeJoinBuffer(middleBuffer);
    freeChunk(outputChunk);

    // Merge all sorted chunks created during the joining process
    // first load all sorted join chunks into tables
    Table* tempTables[numChunks];
    for(int i = 0; i < numChunks; i++) {
        sprintf(tempFilename, "temp_join_%d.bin", i);
        tempTables[i] = loadFileIntoTable(tempFilename, numFieldsResult, true);
        readNextRecord(tempTables[i]);
    }
    // Start merging process
    _mergeTempTables(tempTables, outputFileName, numChunks, index_to_sort_on);

    // Cleanup
    for(int i = 0; i < numChunks; i++) {
        freeTable(tempTables[i]);
        sprintf(tempFilename, "temp_join_%d.bin", i);

        if(remove(tempFilename)!=0) {
            perror("Error deleting file");
        };
    }

    // Load and return result
    Table* result = loadFileIntoTable(outputFileName, numFieldsResult, true);
    return result;
}

// -----------= PRINT FUNCTIONS - FOR TESTING ONLY =----------------

void printRecord(const Record* record, const int numFields) {
    char field[FIELD_LENGTH];
    for (int i = 0; i < numFields; i++) {
        decodeField(record->fields[i], field);
        printf("%s", field);
        if (i < numFields - 1) {
            printf(",");
        }
    }
    printf("\n");
}


void printChunkInfo(const Chunk* chunk) {
    printf("-- N_Records: %d, N_Fields: %d, Capacity %d\n", chunk->numRecords, chunk->numFields, CHUNK_SIZE);
}


void printChunk(const Chunk* chunk) {
    for (int i = 0; i < chunk->numRecords; i++) {
        printRecord(&chunk->records[i], chunk->numFields);
    }
    printChunkInfo(chunk);
}


void printTableInfo(Table* table) {
    printf("--Table at %p, File at %p N_Fields: %d\n", table, table->file, table->numFields);
}


// Print out a limited number of lines from table to console
// CAUTION: THE TABLE CAN NO LONGER BE ACCESSED AFTER PRINTING
void writeTableToConsoleLimited(Table* table) {
    int numPrinted = 0;
    readNextRecord(table);
    while(!table->endOfFile && numPrinted < MAX_PRINT_LINES) {
        printRecord(&table->currentRecord, table->numFields);
        readNextRecord(table);
        numPrinted++;
    }
    if(numPrinted == MAX_PRINT_LINES) {
        printf("..... (more rows)\n");
    }
}

// Print out all number of lines from table to console
// CAUTION: THE TABLE CAN NO LONGER BE ACCESSED AFTER PRINTING
void writeTableToConsole(Table* table) {
    _writeTableToFileStream(table, stdout, false);
}

int main(int argc, char* argv[]) {
    if (argc != 5) {
        printf("Usage: %s <file1> <file2> <file3> <file4>\n", argv[0]);
        return 1;
    }

    Table* table1 = loadFileIntoTable(argv[1], 2, false);
    Table* table2 = loadFileIntoTable(argv[2], 2, false);
    Table* table3 = loadFileIntoTable(argv[3], 2, false);
    Table* table4 = loadFileIntoTable(argv[4], 2, false);

    Table* sortedTable1 = ExternalMergeSort(table1, "s1.csv", 0);
    Table* sortedTable2 = ExternalMergeSort(table2, "s2.csv", 0);
    Table* sortedTable3 = ExternalMergeSort(table3, "s3.csv", 0);

    Table* join123 = joinThreeTables(sortedTable1, sortedTable2, sortedTable3, "j123.csv");
    Table* sortedTable4 = ExternalMergeSort(table4, "s4.csv", 0);

    //writeTableToConsole(join123);

    Table* join1234 = joinTables(join123, sortedTable4, "output-V3.csv");

    writeTableToConsole(join1234);

    // Cleanup
    freeTable(join1234);
    freeTable(join123);

    freeTable(sortedTable4);
    freeTable(sortedTable3);
    freeTable(sortedTable2);
    freeTable(sortedTable1);

    freeTable(table4);
    freeTable(table3);
    freeTable(table2);
    freeTable(table1);

    remove("j123.csv");
    remove("j12.csv");
    remove("s123.csv");
    remove("s1.csv");
    remove("s2.csv");
    remove("s3.csv");
    remove("s4.csv");

    return 0;
}