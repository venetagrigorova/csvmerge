#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#define FIELD_LENGTH 23  // 22 chars + string terminator
#define CHUNK_SIZE 1000000    // Number of records per chunk
#define BUFFER_SIZE 128      // Read/write buffer size
#define MAX_NUM_CHUNKS 30  // Should have CHUNK_SIZE * MAX_NUM_CHUNKS >= 12 Mio.
#define MAX_PRINT_LINES 25 // Only for testing, limit the number of lines printed out at once


// Structure for record with no fixed number of fields
typedef struct {
    char** fields;  // fields is an array of arrays, representing a fixed number of strings
    int numFields;
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
    int numFields;
} Table;

// Buffer structure to hold a varying number of records
// Has key field
// Used internally during join of tables
typedef struct {
    char key[FIELD_LENGTH];
    Record* records;
    int numRecords;
    int numFields;
} JoinBuffer;


typedef struct { // Task structure for threads
    FILE* file;
    int numFields;
    int on_index;
    int chunkIndex;
} SortTask;


// -----------= CONSTUCTORS AND DESTRUCTORS =----------------

// Initialize a record with given number of fields at a certain adress
void initRecord(Record* record, const int numFields) {
    record->numFields = numFields;
    record->fields = (char**)malloc(numFields * sizeof(char*));
    if(!record->fields) {
        perror("Failed to allocate memory for record");
        exit(EXIT_FAILURE);
    }

    // Initialize all fields to empty strings
    for (int i = 0; i < record->numFields; i++) {
        record->fields[i] = (char *)malloc(FIELD_LENGTH * sizeof(char));
        if (!record->fields[i]) {
            perror("Failed to allocate memory for field");
            exit(EXIT_FAILURE);
        }
        memset(record->fields[i], 0, FIELD_LENGTH);
    }
}


// Free memory allocated for a record
void freeRecord(Record* record) {
    for (int i = 0; i < record->numFields; i++) {
        free(record->fields[i]);
    }
    free(record->fields);
}


// Copy to destination record from source record, assuming they have the same number of fields
void copyRecord(Record* dest, const Record* source) {
    // Copy all fields
    if(dest->numFields != source->numFields) {
        perror("Tried to copy to different-length record");
        exit(EXIT_FAILURE);
    }
    for (int i = 0; i < source->numFields; i++) {
        strcpy(dest->fields[i], source->fields[i]);
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
    buffer->key[0] = '\0';
    buffer->records = records;
    buffer->numRecords = 0;
    buffer->numFields = numFields;
    return buffer;
}


bool recordFitsBuffer(Record* record, const int key_index, JoinBuffer* buffer) {
    if(buffer->numRecords == 0) {
        return false;
    }
    int compare = strcmp(buffer->key, record->fields[key_index]);
    return (compare == 0);
}


void writeRecordToBuffer(Record* record, JoinBuffer* buffer, const int right_on) {
    if(buffer->numRecords == 0) {
        strcpy(buffer->key, record->fields[right_on]);
    }
    else if(buffer->numRecords == BUFFER_SIZE) {
        perror("Join buffer overflow - increase BUFFER_SIZE");
        exit(EXIT_FAILURE);
    }
    else if(!recordFitsBuffer(record, right_on, buffer)) {
        perror("Tried to write record to incompatible buffer");
        exit(EXIT_FAILURE);
    }
    copyRecord(&buffer->records[buffer->numRecords], record);
    buffer->numRecords++;
}


void emptyBuffer(JoinBuffer* buffer) {
    buffer->key[0] = '\0';
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

void sanitizeLine(char *line, const int numFields) {
    int len = numFields*FIELD_LENGTH+3;
    size_t i = 1;
    for (i; i < len-1 && line[i] != '\n'; i++) {
        if (line[i] == '\0') {
            line[i] = ' '; // Replace null characters with a space up to the linebreak char
        }
    }
    line[i] = '\0'; // Finally, replace linebreak with null character
}


void loadLineIntoRecord(char* line, const Record* record) {
    // Parse the line into a record by splitting the line on the comma
    char* separator = strtok(line, ",");

    // Copy all fields sequentially
    for(int i=0; i < record->numFields; i++) {
        if (separator == NULL) { // leave this in for testing purposes, remove at the very end
            printf("%s\n", line);
            perror("Found less fields in this line than allocated");
            exit(EXIT_FAILURE);
        }
        strcpy(record->fields[i], separator);
        // advance to next comma
        separator = strtok(NULL, ",");
    }
    if(separator != NULL) { // leave this in for testing purposes, remove at the very end
        printf("%s. %s\n", separator, line);
        perror("Found more fields in this line than allocated");
        exit(EXIT_FAILURE);
    }
}


// Used in Table: Read a single record to replace the current record
void readNextRecord(Table* table) {
    if(table->endOfFile == true) {
        perror("No more records in file");
        exit(EXIT_FAILURE);
    }
    const int length = table->numFields*FIELD_LENGTH + 1; // terminating character
    char line[length];

    if(fgets(line, length, table->file) != NULL ) {
        sanitizeLine(line, table->numFields);
        loadLineIntoRecord(line, &table->currentRecord);
    }
    else { // End of file reached
        table->endOfFile = true;

        // This last section is for debugging and should be removable at the very end of the project
        // It replaces the last currentRecord with the message THE END in every field
        char *errormsg = (char *)malloc(table->numFields * FIELD_LENGTH + 1); // +1 for null terminator
        errormsg[0] = '\0';
        if (errormsg == NULL) {
            perror("Memory allocation failed at last line");
            exit(EXIT_FAILURE);
        }
        for (int i = 0; i < table->numFields; i++) {
            strcat(errormsg, "THE END");
            if (i < table->numFields - 1) {
                strcat(errormsg, ",");
            }
        }
        strcat(errormsg, "\n\0");
        loadLineIntoRecord(errormsg, &table->currentRecord);
        free(errormsg);
    }
}


// read from input file into chunk until chunk is full
Chunk* loadFileIntoChunk(FILE* inputFile, const int numFields) {
    Chunk* chunk = createChunk(CHUNK_SIZE, numFields);

    const int length = numFields*FIELD_LENGTH + 1; // content + separators (, or \n) + terminator
    char line[length];

    int row = 0;
    for(row; row < CHUNK_SIZE; row++) {
        if(feof(inputFile)) {
            row--; // Since we counted one line too much
            break;
        }
        if(fgets(line, length, inputFile) != NULL ) {
            sanitizeLine(line, numFields);
            loadLineIntoRecord(line, &chunk->records[row]);
        }
    }
    chunk->numRecords = row;
    return chunk;
}


Table* loadFileIntoTable(const char* inputFileName, const int numFields) {
    Table* table = createTable(numFields);
    table->file = fopen(inputFileName, "r");
    if(!table->file) {
        perror("Failed to open file");
        exit(EXIT_FAILURE);
    }
    return table;
}



// -----------= OUTPUT FUNCTIONS =----------------

// Write a single record to file
void writeRecordToFile(const Record* record, FILE* output_file) {
    char field[FIELD_LENGTH];
    for (int i = 0; i < record->numFields; i++) {
        int j = 0;
        // Find blank characters and replace them with \0 again
        strcpy(field, record->fields[i]);
        for(j; field[j] != '\0' & j<FIELD_LENGTH; j++) {
            if(field[j] == ' ') {
                field[j] = '\0';
            }

        }
        fwrite(field, sizeof(char), j, output_file);
        if (i < record->numFields - 1) {
            fputc(44, output_file); // put a comma after the field
        }
    }
    fputc(012,output_file); // put a newline after the line
}


// Write an entire chunk to file
void writeChunkToFile(const Chunk* chunk, const char* outputFilename) {
    FILE* file = fopen(outputFilename, "wb+");
    if (!file) {
        perror("Error opening file for writing");
        exit(EXIT_FAILURE);
    }
    for (int i = 0; i < chunk->numRecords; i++) {
        writeRecordToFile(&chunk->records[i], file);
    }
    fclose(file);
}

void _writeTableToFileStream(Table* table, FILE* file) {
    readNextRecord(table);
    while(!feof(table->file)) {
        writeRecordToFile(&table->currentRecord, file);
        readNextRecord(table);
    }
}

// Write an entire table to file
void writeTableToFile(Table* table, const char* outputFilename) {
    FILE* file = fopen(outputFilename, "wb+");
    if (!file) {
        perror("Error opening file for writing");
        exit(EXIT_FAILURE);
    }
    _writeTableToFileStream(table, file);
    fclose(file);
}



// -----------= SORTING FUNCTIONS =----------------

// Compare records based on left and right field
int compareRecordsOnFields(const void* left, const void* right, const int left_on, const int right_on) {
    const Record* r1 = (const Record*)left;
    const Record* r2 = (const Record*)right;
    return strcmp(r1->fields[left_on], r2->fields[right_on]);
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

void* sortChunkThread(void* arg) {
    SortTask* task = (SortTask*)arg;
    char tempFilename[32];
    sprintf(tempFilename, "temp_%d.bin", task->chunkIndex);

    Chunk* chunk = loadFileIntoChunk(task->file, task->numFields);
    sortChunk(chunk, task->on_index);
    writeChunkToFile(chunk, tempFilename);
    freeChunk(chunk);

    return NULL;
}


Table* ExternalMergeSort(const Table* table, const char* outputFileName, const int on_index) {
    int numChunks = 0;
    pthread_t threads[MAX_NUM_CHUNKS];
    SortTask tasks[MAX_NUM_CHUNKS];

    // Process the table in chunks, each chunk is sorted in a separate thread
    while (!feof(table->file) && numChunks < MAX_NUM_CHUNKS) {
        // Prepare task details for the current chunk
        tasks[numChunks].file = table->file;
        tasks[numChunks].numFields = table->numFields;
        tasks[numChunks].on_index = on_index; // Field index to sort on
        tasks[numChunks].chunkIndex = numChunks;

        // Create a thread to sort the current chunk
        pthread_create(&threads[numChunks], NULL, sortChunkThread, &tasks[numChunks]);
        numChunks++;
    }

    // Wait for all sorting threads to complete
    for (int i = 0; i < numChunks; i++) {
        pthread_join(threads[i], NULL);
    }

    // Open the output file for writing the merged and sorted results
    FILE* output = fopen(outputFileName, "wb+");
    if (!output) {
        perror("Error opening output file for writing");
        exit(EXIT_FAILURE);
    }

    // Load sorted chunks from temporary files into memory
    Table* tempTables[MAX_NUM_CHUNKS];
    for (int i = 0; i < numChunks; i++) {
        char tempFilename[32];
        sprintf(tempFilename, "temp_%d.bin", i);
        tempTables[i] = loadFileIntoTable(tempFilename, table->numFields);
        readNextRecord(tempTables[i]); // Read the first record from each table
    }

    // Perform a k-way merge to combine sorted chunks
    while (true) {
        int minRecordIndex = -1; // Index of the current minimum record across chunks

        // Find the smallest record among the chunks
        for (int i = 0; i < numChunks; i++) {
            if (!tempTables[i]->endOfFile && (minRecordIndex == -1 ||
                compareRecordsOnFields(&tempTables[i]->currentRecord,
                                       &tempTables[minRecordIndex]->currentRecord, on_index, on_index) < 0)) {
                minRecordIndex = i;
            }
        }

        // If no more records are available, exit the merge loop
        if (minRecordIndex == -1) {
            break;
        }

        // Write the smallest record to the output file
        writeRecordToFile(&tempTables[minRecordIndex]->currentRecord, output);

        // Advance to the next record in the chunk containing the smallest record
        readNextRecord(tempTables[minRecordIndex]);
    }

    fclose(output);

    // Clean up
    for (int i = 0; i < numChunks; i++) {
        freeTable(tempTables[i]);
        char tempFilename[32];
        sprintf(tempFilename, "temp_%d.bin", i);
        if (remove(tempFilename) != 0) {
            perror("Error deleting file");
        }
    }

    // Load and return the final sorted table from the output file
    return loadFileIntoTable(outputFileName, table->numFields);
}

//  -----------= JOIN FUNCTIONS =----------------

Record* mergeTwoRecords(const Record* left, const Record* right, const int left_on, const int right_on) {
    const int numFields = left->numFields + right->numFields - 1;
    Record* result = malloc(sizeof(Record));
    int resultIndex = 0;
    if(!result) {
        perror("Error allocating memory for result");
        exit(EXIT_FAILURE);
    }
    initRecord(result, numFields);
    if(compareRecordsOnFields(left, right, left_on, right_on) != 0) {
        perror("Tried to merge two incompatible records");
        exit(EXIT_FAILURE);
    }

    result->fields[resultIndex] = left->fields[left_on]; // Result always starts with field that we are joining on
    resultIndex++;
    // Copy left fields (expect join field)
    for(int i = 0; i < left->numFields; i++) {
        if(i!=left_on) {
            result->fields[resultIndex] = left->fields[i];
            resultIndex++;
        }
    }
    // Copy right fields (except join field)
    for(int i = 0; i < right->numFields; i++) {
        if(i!=right_on) {
            result->fields[resultIndex] = right->fields[i];
            resultIndex++;
        }
    }
    return result;
}


Table* joinTables(Table* left, Table* right, const int left_on, const int right_on, const char* outputFileName) {
    FILE* output = fopen(outputFileName, "wb+");
    if(!output) {
        perror("Error opening output file for writing");
        exit(EXIT_FAILURE);
    }

    JoinBuffer* buffer = createJoinBuffer(right->numFields);

    readNextRecord(left);
    readNextRecord(right);
    while(! (left->endOfFile && right->endOfFile )) {
        int comparison = compareRecordsOnFields(&left->currentRecord, &right->currentRecord, left_on, right_on);
        if(comparison == 0 && (!left->endOfFile) && (!right->endOfFile)) {
            // Load right record into buffer
            writeRecordToBuffer(&right->currentRecord, buffer, right_on);

            // Write the found joined records to output
            Record* tempRecord = mergeTwoRecords(&left->currentRecord,
                                                 &right->currentRecord,
                                                 left_on, right_on);
            writeRecordToFile(tempRecord, output);
            readNextRecord(right);
        }
        else if ((comparison < 0 && !left->endOfFile) || right->endOfFile) {
            readNextRecord(left);

            if(recordFitsBuffer(&left->currentRecord, left_on, buffer)) {
                // In this case we can match the left record to all previous right records that were loaded
                // into the buffer. For each of the Records in the buffer output a join result of the current left
                // row with one previous right row.
                for(int i = 0; i < buffer->numRecords; i++) {
                    Record* tempRecord = mergeTwoRecords(&left->currentRecord,
                                                         &buffer->records[i],
                                                         left_on, right_on);
                    writeRecordToFile(tempRecord, output);
                }
            }
            else if(buffer->numRecords != 0) {
                // In this case we don't need the buffer anymore
                emptyBuffer(buffer);
            }
        }
        else if (!right->endOfFile) {
            readNextRecord(right);
        }
        else { // Program should never land here
            perror("Could not read any more records");
            exit(EXIT_FAILURE);
        }
    }
    fclose(output);
    freeJoinBuffer(buffer);

    const int numFieldsResult = left->numFields + right->numFields - 1; // Subtract the one common join field
    Table* result = loadFileIntoTable(outputFileName, numFieldsResult);
    return result;
}



// -----------= PRINT FUNCTIONS - FOR TESTING ONLY =----------------

void printRecord(const Record* record) {
    char field[FIELD_LENGTH];
    for (int i = 0; i < record->numFields; i++) {
        // Find blank characters and replace them with \0 again
        strcpy(field, record->fields[i]);
        char *space_ptr = strchr(field, ' ');
        bool has_weird_space_issue = (space_ptr != NULL);
        if (has_weird_space_issue) {
            *space_ptr = '\0';
        }
        printf("%s", field);
        if (i < record->numFields - 1) {
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
        printRecord(&chunk->records[i]);
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
        printRecord(&table->currentRecord);
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
    _writeTableToFileStream(table, stdout);
}



int main(int argc, char* argv[]) {
    if (argc != 5) {
        printf("Usage: %s <file1> <file2> <file3> <file4>\n", argv[0]);
        return 1;
    }

    Table* table1 = loadFileIntoTable(argv[1], 2);
    Table* table2 = loadFileIntoTable(argv[2], 2);
    Table* table3 = loadFileIntoTable(argv[3], 2);
    Table* table4 = loadFileIntoTable(argv[4], 2);

    Table* sortedTable1 = ExternalMergeSort(table1, "s1.csv", 0);
    Table* sortedTable2 = ExternalMergeSort(table2, "s2.csv", 0);

    Table* join12 = joinTables(sortedTable1, sortedTable2, 0, 0, "j12.csv");

    Table* sortedTable3 = ExternalMergeSort(table3, "s3.csv", 0);
    Table* join123 = joinTables(join12, sortedTable3, 0, 0, "j123.csv");

    Table* sortedTable4 = ExternalMergeSort(table4, "s4.csv", 0);
    Table* sortedJoin123 = ExternalMergeSort(join123, "s123.csv", 3);

    Table* join1234 = joinTables(sortedJoin123, sortedTable4, 3, 0, "output-base.csv");

    writeTableToConsole(join1234);

    // Cleanup
    freeTable(join1234);
    freeTable(join123);
    freeTable(join12);

    freeTable(sortedJoin123);
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