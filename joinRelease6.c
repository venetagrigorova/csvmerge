#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


// Put all implemented performance optimizations here (relative to base.c)
// Currently implemented in this file
// - Threeway merge join implemented
// - created utility functions copyField, _mergeTempTables
// - Remove numFields from Record struct, thus mostly passed as input parameter
// - Used INT128 encoding, major overhauls to low-level functions
// - Used "static inline" on all minor utility functions
// - Store Fields of one chunk continuously in memory using chunk->memoryBlock
// - Tables now have a memory buffer going backwards and forwards
// - Eliminated Buffers by using internal table buffer instead
// - Changed it that the last merge directly prints the result instead of saving a file
// - compareRecordsOnFields - early return + restricted
// - Added Lookup-tables for encoding and decoding
// - Added k-way merge algorithm
// - QuickSort algorithm have been optimized

#define FIELD_LENGTH 23  // 22 chars + string terminator
#define CHUNK_SIZE 100001    // Number of records per chunk
#define MAX_NUM_CHUNKS 140  // Should have CHUNK_SIZE * MAX_NUM_CHUNKS >= 12 Mio.
#define MAX_PRINT_LINES 70 // Only for testing, limit the number of lines printed out at once
#define MAX_NUM_CHARACTERS 55 // 1 + 10 + 26 + 17 + 1 (can handle \0, 0-9, A-Z, a-q, and one unknown character)
#define INT_128 unsigned __int128
#define TABLE_SIZE 2048         // How many records to store in a table at any time
#define TABLE_BUFFER_SIZE 64  // How many of the records in the buffer to keep for backtracking, should be >128, << TABLE_SIZE


// Structure for record with no fixed number of fields
typedef struct {
    INT_128* fields;  // fields is an array of unsigned int128, each representing one field
} Record;

// Structure for managing chunks fully loaded into memory
// Has an array of records, and number of records and fields of each record
typedef struct {
    Record* records;
    INT_128* memory_block;
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
    INT_128* memory_block; // New: Table has internal memory
    int currentPos;
    int numRecords;
} Table;

// Lookup table
static unsigned int encodingTable[128] = {0};
static char decodingTable[MAX_NUM_CHARACTERS] = {0};

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
static inline void freeRecord(Record* record) {
    if(!record) return;
    if(record->fields) free(record->fields);
    if(record) record = NULL;
}

static inline void copyField(const Record* dest, const Record* source, const int dest_index, const int source_index) {
    dest->fields[dest_index] = source->fields[source_index];
}

// Create and initialize a chunk
Chunk* createChunk(const int numRecords, const int numFields) {
    Chunk* chunk = malloc(sizeof(Chunk));
    Record* records = malloc(numRecords * sizeof(Record));
    // Allocate a contiguous block of memory for all fields of all records
    INT_128* fieldBlock = malloc(numRecords * numFields * sizeof(INT_128));

    if (!chunk | !records | !fieldBlock) {
        perror("Failed to allocate memory for chunk");
        exit(EXIT_FAILURE);
    }

    // Loop unrolling here?
    for (int i = 0; i < numRecords; i++) {
        records[i].fields = fieldBlock + i * numFields;
    }
    chunk->records = records;
    chunk->numRecords = 0;
    chunk->memory_block = fieldBlock;
    chunk->numFields = numFields;
    return chunk;
}

static void freeChunk(Chunk* chunk) {
    if (!chunk) return;
    if (chunk->memory_block != NULL) {
        free(chunk->memory_block);
    }
    if (chunk->records != NULL) {
        free(chunk->records);
    }
    free(chunk);
    chunk = NULL;
}

// Create and initialize a table
Table* createTable(const int numFields) {
    Table* table = malloc(sizeof(Table));
    table->memory_block = malloc(TABLE_SIZE * numFields * sizeof(INT_128));
    if(!table | !table->memory_block) {
        perror("Failed to allocate memory for table");
        exit(EXIT_FAILURE);
    };
    table->file = NULL;
    table->numFields = numFields;
    table->endOfFile = 0;
    table->file_is_encoded = false;

    Record* currentRecord = malloc(sizeof(Record));
    initRecord(currentRecord, numFields);
    table->currentRecord = *currentRecord;
    table->currentPos = 0;
    table->numRecords = 0;
    return table;
}


// Free memory allocated for a table
static inline void freeTable(Table* table) {
    if (!table) return;
    if (table->memory_block)  free(table->memory_block);
    if (table->file) fclose(table->file);
    free(table);
    table = NULL;
}


// -----------= INPUT FROM FILE INTO TABLE/CHUNK =----------------

static void initializeEncodingTable() {
    for (char c = 'A'; c <= 'Z'; c++) encodingTable[(int)c] = c - 'A' + 1; // 'A' -> 1, 'B' -> 2, ..., 'Z' -> 26
    for (char c = '0'; c <= '9'; c++) encodingTable[(int)c] = c - '0' + 27; // '0' -> 27, '1' -> 28, ..., '9' -> 36
    for (char c = 'a'; c <= 'z'; c++) {
        int temp = c - 'a' + 37; // 'a' -> 37, ..., 'q' -> 53
        if(temp >= MAX_NUM_CHARACTERS) temp = MAX_NUM_CHARACTERS - 1;
        encodingTable[(int)c] = temp;
    }
    encodingTable[(int)'\0'] = MAX_NUM_CHARACTERS - 1;

    for (int i = 1; i <= 26; i++) decodingTable[i] = 'A' + i - 1; // 1 -> 'A', 2 -> 'B', ..., 26 -> 'Z'
    for (int i = 27; i <= 36; i++) decodingTable[i] = '0' + i - 27; // 27 -> '0', ..., 36 -> '9'
    for (int i = 37; i <= MAX_NUM_CHARACTERS-2; i++) decodingTable[i] = 'a' + i - 37;
    decodingTable[MAX_NUM_CHARACTERS-1] = '\0';
}

static inline INT_128 encodeField(const char* restrict field) {
    INT_128 result = 0;
    INT_128 offset = 1;
    char character;
    unsigned int add = 0;
    for (int i = 0; i < FIELD_LENGTH && field[i] != ',' && field[i] != '\n'; i++) {
        character = field[i];

        add = encodingTable[(int)character];  // Lookup instead of if-else

        result += add * offset;
        offset *= MAX_NUM_CHARACTERS;
    }
    return result;
}

// decodes Int128 into output string and returns the length (not including terminating null but
// possibly including custom null character in string)
static int decodeField(INT_128 code, char* output) {
    INT_128 value = code;
    unsigned int remainder = 0;
    int len = 0;
    for(len = 0; len < FIELD_LENGTH; len++) {
        remainder = value % MAX_NUM_CHARACTERS;

        output[len] = decodingTable[remainder];

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


static inline void loadLineIntoRecord(const char* line, const Record* record, const int numFields) {
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


// Refill table buffer while keeping the most recent records
// Example:
// prior          after
//
//    R      B     B
//    B    ->c   ->c
//  ->c            N
//
//  Where R and N are a huge number of records, B&c is the small number of records that will be preserved,
// and c is the current and d the next record
static void refillBuffer(Table* table) {
    // This handles the very first fill of the table memory, where we dont need a buffer
    if(table->numRecords == 0) {
        size_t recordsRead = fread(table->memory_block,
                                    sizeof(INT_128) * table->numFields,
                                    TABLE_SIZE,
                                    table->file
                                    );
        table->numRecords = recordsRead;
        return;
    }

    //ELSE:
    int buffer_size = TABLE_BUFFER_SIZE;
    // for edge case where table is really small
    if (buffer_size > table->numRecords) buffer_size = table->numRecords;
    int records_toload = (table->numRecords - buffer_size);

    // Copy the last BUFFER_SIZE records to the beginning of the buffer
    // This preserves the most recent BUFFER_SIZE records
    // Plus the last record in the memory, which is the next record to be read
    memmove(table->memory_block,
            table->memory_block + (records_toload * table->numFields),
            buffer_size * table->numFields * sizeof(INT_128));

    // Read new records into the rest of the buffer
    size_t recordsRead = fread(
        table->memory_block + (buffer_size * table->numFields),
        sizeof(INT_128) * table->numFields,
        records_toload,
        table->file
    );

    if (recordsRead == 0)   table->endOfFile = true;

    // Set the current table position to the start of the new records
    table->currentPos = buffer_size;
    table->numRecords = buffer_size + recordsRead;
}

static INT_128 peekPreviousField(const Table* table, const int offset, const int Field) {
    if(offset < 0 || 0*offset >= TABLE_BUFFER_SIZE) {
        perror("PeekPreviousRecord: invalid offset encountered");
        exit(EXIT_FAILURE);
    }
    if(Field < 0 || Field >= table->numFields) {
        perror("PeekPreviousRecord: invalid Field encountered");
        exit(EXIT_FAILURE);
    }
    size_t peekIndex = table->currentPos - 1 - offset;
    INT_128 result = *(table->memory_block + peekIndex*table->numFields + Field); //table->memory_block[(peekIndex * table->numFields) + Field];
    return result;
}

static inline void readNextRecord(Table* table) {
    if(table->endOfFile == true) {
        perror("No more records in file");
        exit(EXIT_FAILURE);
    }
    // If buffer is empty or we've reached the end of the current table memory, refill buffer
    if (table->currentPos == table->numRecords) {

        refillBuffer(table);

    }
    // Otherwise, point currentRecord's fields to the next set of fields in the buffer and increment position
    table->currentRecord.fields = table->memory_block + (table->currentPos * table->numFields);
    table->currentPos++;
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
    table->file = fopen(inputFileName, "rb");
    table->file_is_encoded = file_is_encoded;
    if(!table->file) {
        perror("Failed to open file");
        exit(EXIT_FAILURE);
    }
    return table;
}



// -----------= OUTPUT FUNCTIONS =----------------

// Write a single record to file
static inline void writeRecordToFile(const Record* record, FILE* output_file, const int numFields, bool encode_file) {
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

static inline void writeChunkToFileStream(const Chunk* chunk, FILE* file) {
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
    writeChunkToFileStream(chunk, file);
    fclose(file);
}

static inline void writeTableToFileStream(Table* table, FILE* file, bool encode_file) {
    readNextRecord(table);
    while(!table->endOfFile) {
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
    writeTableToFileStream(table, file, encode_file);
    fclose(file);
}

// Print out all number of lines from table to console
// CAUTION: THE TABLE CAN NO LONGER BE ACCESSED AFTER PRINTING
static inline void writeTableToConsole(Table* table) {
    writeTableToFileStream(table, stdout, false);
}



// -----------= SORTING FUNCTIONS =----------------

// Compare records based on left and right field
static inline int compareRecordsOnFields(const void* restrict left, const void* restrict right, const int left_on, const int right_on) {
    const Record* r1 = (const Record*)left;
    const Record* r2 = (const Record*)right;

    if(r1->fields[left_on] > r2->fields[right_on]) return 1;
    if(r1->fields[left_on] < r2->fields[right_on]) return -1;

    return 0;
}

// Inlined function to swap two records
static inline void swapRecords(Record* a, Record* b) {
    Record tmp = *a;
    *a = *b;
    *b = tmp;
}

// Quicksort specialized for on_index = 0
//Didn't make it inline (not beneficial) instead swapRecords functions is inlined
static void quicksortOnZero(Record* arr, int left, int right) {
    if (left >= right) return;

    // Pick a pivot (for simplicity, the middle element)
    int mid = (left + right) / 2;
    INT_128 pivotVal = arr[mid].fields[0];

    int i = left;
    int j = right;
    while (i <= j) {
        // Move 'i' forward while the key is < pivotVal
        while (arr[i].fields[0] < pivotVal) i++;
        // Move 'j' backward while the key is > pivotVal
        while (arr[j].fields[0] > pivotVal) j--;

        if (i <= j) {
            swapRecords(&arr[i], &arr[j]);
            i++;
            j--;
        }
    }
    // Recurse on left partition
    if (left < j) quicksortOnZero(arr, left, j);
    // Recurse on right partition
    if (i < right) quicksortOnZero(arr, i, right);
}

// Quicksort specialized for on_index = 3
static void quicksortOnThird(Record* arr, int left, int right) {
    if (left >= right) return;

    int mid = (left + right) / 2;
    INT_128 pivotVal = arr[mid].fields[3];

    int i = left;
    int j = right;
    while (i <= j) {
        while (arr[i].fields[3] < pivotVal) i++;
        while (arr[j].fields[3] > pivotVal) j--;

        if (i <= j) {
            swapRecords(&arr[i], &arr[j]);
            i++;
            j--;
        }
    }
    if (left < j) quicksortOnThird(arr, left, j);
    if (i < right) quicksortOnThird(arr, i, right);
}

void sortChunk(Chunk* chunk, int on_index) {
    // If there's 0 or 1 record, nothing to sort
    if (chunk->numRecords <= 1) {
        return;
    }

    if (on_index == 0) {
        // Sort by fields[0]
        quicksortOnZero(chunk->records, 0, chunk->numRecords - 1);
    }
    else if (on_index == 3) {
        // Sort by fields[3]
        quicksortOnThird(chunk->records, 0, chunk->numRecords - 1);
    }
    else {
        perror("Not implemented");
        exit(EXIT_FAILURE);
    }
}

// Node structure for the tournament tree
typedef struct {
    int chunkIndex;    // Index of the chunk this record came from
    Record* record;      // Pointer to the actual record
    bool isValid;      // Flag to indicate if this node contains valid data
} TreeNode;

// Tournament tree structure
typedef struct {
    TreeNode* nodes;   // Array to store the tree nodes
    int numLeaves;     // Number of leaf nodes (equal to number of chunks)
    int totalNodes;    // Total number of nodes in the tree
    int* leafToNode;   // Mapping from chunk index to leaf node index
} TournamentTree;

// Function to get the parent index
static inline int parent(int i) {
    return (i - 1) / 2;
}

// Function to get left child index
static inline int leftChild(int i) {
    return 2 * i + 1;
}

// Initialize the tournament tree
TournamentTree* initTournamentTree(int numChunks) {
    TournamentTree* tree = (TournamentTree*)malloc(sizeof(TournamentTree));
    tree->numLeaves = 1;
    while (tree->numLeaves < numChunks) {
        tree->numLeaves *= 2;
    }

    // Calculate total nodes in the complete binary tree
    tree->totalNodes = 2 * tree->numLeaves - 1;

    // Allocate memory for nodes
    tree->nodes = (TreeNode*)calloc(tree->totalNodes, sizeof(TreeNode));

    // Initialize mapping from chunk index to leaf node index
    tree->leafToNode = (int*)malloc(numChunks * sizeof(int));
    for (int i = 0; i < numChunks; i++) {
        tree->leafToNode[i] = tree->totalNodes - tree->numLeaves + i;
    }
    return tree;
}

// Update a leaf node and propagate the winner up the tree
void updateNode(TournamentTree* tree, const int chunkIndex, Record* record, const int on_index, bool is_valid) {
    int nodeIndex = tree->leafToNode[chunkIndex];

    // Update leaf node
    tree->nodes[nodeIndex].chunkIndex = chunkIndex;
    tree->nodes[nodeIndex].record = record;
    tree->nodes[nodeIndex].isValid = is_valid;

    // Propagate up the tree
    while (nodeIndex > 0) {
        int parentIndex = parent(nodeIndex);
        int siblingIndex = (nodeIndex % 2 == 0) ? nodeIndex - 1 : nodeIndex + 1;

        TreeNode* current = &tree->nodes[nodeIndex];
        TreeNode* sibling = &tree->nodes[siblingIndex];
        TreeNode* parent = &tree->nodes[parentIndex];

        // If sibling is invalid or current node has smaller key
        if (!sibling->isValid ||
            (current->isValid && sibling->isValid &&  0 > compareRecordsOnFields(current->record, sibling->record, on_index, on_index))
            ) {
            *parent = *current;
            } else {
                *parent = *sibling;
            }

        nodeIndex = parentIndex;
    }
}

// Main k-way merge function using tournament tree
void _mergeTempTables(Table** tempTables, const char* outputFileName, const int numChunks, const int on_index) {
    FILE* output = fopen(outputFileName, "wb+");
    if(!output) {
        perror("Error opening output file for writing");
        exit(EXIT_FAILURE);
    }

    int numFields = tempTables[0]->numFields;
    TournamentTree* tree = initTournamentTree(numChunks);

    for (int i = 0; i < numChunks; i++) {
        updateNode(tree, i, &tempTables[i]->currentRecord,
                 on_index, true);
    }

    // Merge phase
    // Combine temp Tables
    // 1) Compare the current records of all active temp tables
    // 2) Find out which is the smallest
    // 3) Print that record into the output file
    // 4) Repeat until all records are processed
    while (tree->nodes[0].isValid) {
        // Get the winner (minimum record)
        int winnerChunkIndex = tree->nodes[0].chunkIndex;
        Record* winnerRecord = tree->nodes[0].record;

        writeRecordToFile(winnerRecord, output, numFields, true);
        readNextRecord(tempTables[winnerChunkIndex]);

        // Update the tournament tree with the new record
        if (!tempTables[winnerChunkIndex]->endOfFile) {
            updateNode(tree, winnerChunkIndex,
                      &tempTables[winnerChunkIndex]->currentRecord,
                      on_index, true);
        } else {
            updateNode(tree, winnerChunkIndex, NULL, on_index, false);
        }
    }
    fclose(output);
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


//  -----------= JOIN FUNCTIONS =----------------

static inline void _mergeTwoRecords(const Table* left, const Table* right, const int right_offset,
                        const Record* target) {
    // write as string
    char field[FIELD_LENGTH];
    int length = 0;
    length = decodeField(left->currentRecord.fields[3], field);
    fwrite(field, sizeof(char), length, stdout);          // D
    fputc(44, stdout); // put a comma after the field

    for (int i = 0; i <= 2; i++) {     // A, B, C
        length = decodeField(left->currentRecord.fields[i], field);
        fwrite(field, sizeof(char), length, stdout);
        fputc(44, stdout); // put a comma after the field
    }

    length = decodeField(peekPreviousField(right, right_offset, 1), field);
    fwrite(field, sizeof(char), length, stdout);     // E
    fputc(012, stdout); // put a newline after the line
}

static inline void _mergeThreeRecordsIntoRecord(const Table* left, const Table* middle, const Table* right,
                                  const int middle_offset, const int right_offset, const Record* target)
{
    copyField(target, &left->currentRecord, 0, 0 ); // A
    copyField(target, &left->currentRecord, 1, 1);  // B
    target->fields[2] = peekPreviousField(middle, middle_offset, 1); // C
    target->fields[3] = peekPreviousField(right, right_offset, 1); // D
}

// Just a wrapper for _mergeThreeRecordsIntoRecord
void processThreeRecords(const Table* left, const Table* middle, const Table* right,
                          const int middle_offset, const int right_offset,
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

    _mergeThreeRecordsIntoRecord(left, middle, right, middle_offset, right_offset, outputRecord);
    outputChunk->numRecords++;
}


void joinTables(Table* left, Table* right) {
    const int numFieldsOutput = 5;
    Record* tempRecord = malloc(sizeof(Record));
    initRecord(tempRecord, numFieldsOutput);

    int n_buffered = 0;
    Record* bufferRecord = malloc(sizeof(Record));
    initRecord(bufferRecord, 2);

    bool is_in_join_phase = false;

    readNextRecord(left);
    readNextRecord(right);
    while(!left->endOfFile) {
        int comparison = compareRecordsOnFields(&left->currentRecord, &right->currentRecord, 3, 0);
        if(right->endOfFile) {
            comparison = -1;
        }

        if(comparison == 0) {
            if(!is_in_join_phase) {
                n_buffered = 0;
                is_in_join_phase = true;
                INT_128 key = *(right->currentRecord.fields);
                bufferRecord->fields[0] = key;
            }
            // Load right record into buffer
            n_buffered++;

            // Write the found joined records to output
            _mergeTwoRecords(left, right, 0, tempRecord);
            //writeRecordToFile(tempRecord, output, numFieldsOutput, true);

            // Advance right table
            readNextRecord(right);
        }
        else if (comparison < 0) {
            readNextRecord(left);
            if(!is_in_join_phase | left->endOfFile) {
                continue;
            }
            if( compareRecordsOnFields(&left->currentRecord, bufferRecord, 3, 0) == 0) {
                // In this case we can match the left record to all previous right records that were loaded
                // into the buffer. For each of the Records in the buffer output a join result of the current left
                // row with one previous right row.
                for(int i = 1; i <= n_buffered; i++) {
                    _mergeTwoRecords(left, right, i, tempRecord);
                    //writeRecordToFile(tempRecord, output, numFieldsOutput, true);
                }
            }
            else {
                // In this case we don't need the buffer anymore
                n_buffered = 0;
                is_in_join_phase = false; //end of current join phase
            }
        }
        else {
            readNextRecord(right);
        }
    }
    freeRecord(tempRecord);
    return;
}


Table* joinThreeTables(Table* left, Table* middle, Table* right,
                        const char* outputFileName) {

    Record* bufferRecordMiddle = malloc(sizeof(Record));
    Record* bufferRecordRight = malloc(sizeof(Record));
    initRecord(bufferRecordMiddle, 2);
    initRecord(bufferRecordRight, 2);
    int n_buffered_middle = 0;
    int n_buffered_right = 0;

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
            if(!is_in_join_phase) {
                n_buffered_right = 0;
                is_in_join_phase = true;
                bufferRecordRight->fields[0] = *right->currentRecord.fields;
            }
            // Load right record into buffer
            n_buffered_right++;

            // Write the found joined records to output
            processThreeRecords(left, middle, right, 0, 0,
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
                n_buffered_middle++;
                bufferRecordMiddle->fields[0] = *middle->currentRecord.fields;
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
            if(compareRecordsOnFields(&middle->currentRecord, bufferRecordRight, 0, 0) == 0) { // recordFitsBuffer(&middle->currentRecord, 0, rightBuffer)) {
                // In this case we can match the left, middle records to all previous right records that were loaded
                // into the buffer. For each of the Records in the buffer output a join result of the current left
                // row and middle row with one previous right row.
                for(int i = 1; i <= n_buffered_right; i++) {
                    processThreeRecords(left, middle, right, 0, i,
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
            if(compareRecordsOnFields(&left->currentRecord, bufferRecordMiddle, 0, 0) == 0) { //recordFitsBuffer(&left->currentRecord, 0, middleBuffer)) {
                for(int i = 1; i <= n_buffered_middle; i++) {
                    for(int j = 1; j <= n_buffered_right; j++) {
                        processThreeRecords(left, middle, right, i, j, outputChunk, &numChunks);
                    }
                }
            }
            else { // Since the next left record did not fit the patter, this is the end of the current join phase
                is_in_join_phase = false;
                n_buffered_middle = 0;
                n_buffered_right = 0;
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
    freeChunk(outputChunk);
    freeRecord(bufferRecordRight);
    freeRecord(bufferRecordMiddle);

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


int main(int argc, char* argv[]) {
    if (argc != 5) {
        printf("Usage: %s <file1> <file2> <file3> <file4>\n", argv[0]);
        return 1;
    }
    initializeEncodingTable();

    Table* table1 = loadFileIntoTable(argv[1], 2, false);
    Table* table2 = loadFileIntoTable(argv[2], 2, false);
    Table* table3 = loadFileIntoTable(argv[3], 2, false);
    Table* table4 = loadFileIntoTable(argv[4], 2, false);

    Table* sortedTable1 = ExternalMergeSort(table1, "s1.csv", 0);
    Table* sortedTable2 = ExternalMergeSort(table2, "s2.csv", 0);
    Table* sortedTable3 = ExternalMergeSort(table3, "s3.csv", 0);

    Table* join123 = joinThreeTables(sortedTable1, sortedTable2, sortedTable3, "j123.csv");
    Table* sortedTable4 = ExternalMergeSort(table4, "s4.csv", 0);

    // This also prints the tables now to console, and does not save files anymore
    joinTables(join123, sortedTable4);

    // Cleanup
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