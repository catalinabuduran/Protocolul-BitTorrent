#include <mpi.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#define TRACKER_RANK 0
#define MAX_FILES 10
#define MAX_FILENAME 15
#define HASH_SIZE 32
#define MAX_CHUNKS 100
#define MAX_CLIENTS 100

#define TRACKER_REQUEST_TAG 2
#define TRACKER_FILENAME_TAG 4
#define TRACKER_SEGMENT_NUMBER_TAG 5
#define TRACKER_SEEDS_NUMBER_TAG 6
#define TRACKER_INDEX_TAG 7
#define TRACKER_SEEDS_TAG 8
#define TRACKER_OWNED_FILES_NUMBER 9
#define CHUNK_TAG 15

//tracker
typedef struct {
    char hash[HASH_SIZE];
    int seeds[MAX_CLIENTS];
    int seedsNumber;
    int index;
    int lastUsedSeed;
} SegmentSwarm;

typedef struct {
    char filename[MAX_FILENAME];
    SegmentSwarm segmentSwarms[MAX_CHUNKS];
    int segmentSwarmsNumber;
} FileSwarm;

//client
typedef struct {
    char filename[MAX_FILENAME];
    int segmentsReceived[MAX_CHUNKS];
    int segmentsReceivedNumber;
    SegmentSwarm segmentsSwarms[MAX_CHUNKS];
} desiredFile;


typedef struct {
    int rank;                     
    FileSwarm files_owned[MAX_FILES]; 
    int files_owned_count;   
    desiredFile desired_files[MAX_FILES];
    int desired_files_number; 
    int files_requested_completed;
} Client;

FileSwarm files[MAX_FILES]; 
Client *clients = NULL;
int clientsNumber = 0;
int numtasks = 0;
int fileSwarmsLength = 0;


int getRoundRobinSeed(SegmentSwarm *segments, int segmentIndex) {
    if (segments[segmentIndex].seedsNumber > 0) {
        int nextSeed = (segments[segmentIndex].lastUsedSeed + 1) % segments[segmentIndex].seedsNumber;
        segments[segmentIndex].lastUsedSeed = nextSeed;

        return segments[segmentIndex].seeds[nextSeed];
    }

    fprintf(stderr, "Error: No seeds available for segment %d\n", segmentIndex);
    return -1;
}

void send_request_to_tracker(int rank, const char *filename) {
    if (!filename) {
        fprintf(stderr, "Error: Invalid filename.\n");
        return;
    }

    printf("Peer %d sending request for file: %s\n", rank, filename);

    const char *owners_request = "req seeds";
    MPI_Send(owners_request, strlen(owners_request) + 1, MPI_CHAR, TRACKER_RANK, TRACKER_REQUEST_TAG, MPI_COMM_WORLD);
    MPI_Send(filename, MAX_FILENAME, MPI_CHAR, TRACKER_RANK, TRACKER_FILENAME_TAG, MPI_COMM_WORLD);
}

void send_segment_request_to_seed(int rank, int file_index, SegmentSwarm *segments, const char *filename, int segmentIndex,  Client *client) {
    if (!filename || !segments || segmentIndex < 0) {
        fprintf(stderr, "Error: Invalid parameters for sending segment request.\n");
        return;
    }
    int seed;
    const char *request = "req hash";

    seed = getRoundRobinSeed(segments, segmentIndex);
    if (seed == -1) {
        fprintf(stderr, "Error: Failed to get a valid seed for segment %d\n", segmentIndex);
        return;
    }

    MPI_Send(request, strlen(request) + 1, MPI_CHAR, seed, TRACKER_REQUEST_TAG, MPI_COMM_WORLD);
    MPI_Send(client->desired_files[file_index].filename, MAX_FILENAME, MPI_CHAR, seed, TRACKER_FILENAME_TAG, MPI_COMM_WORLD);
    MPI_Send(&segments[segmentIndex].index, 1, MPI_INT, seed, TRACKER_INDEX_TAG, MPI_COMM_WORLD);
}

void receive_segment_from_seed(int file_index, Client *client, int segment_index, char *hash, int seed) {
    if (!hash || file_index < 0 || segment_index < 0) {
        fprintf(stderr, "Error: Invalid parameters for receiving segment.\n");
        return;
    }

    MPI_Recv(hash, HASH_SIZE + 1, MPI_CHAR, seed, CHUNK_TAG, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

    SegmentSwarm *segment = &client->desired_files[file_index].segmentsSwarms[segment_index];
    client->desired_files[file_index].segmentsReceived[segment_index] = seed;
    client->desired_files[file_index].segmentsReceivedNumber++;

    strncpy(segment->hash, hash, HASH_SIZE);
    segment->hash[HASH_SIZE] = '\0';
    segment->index = segment_index;

}

void send_ready_request_to_tracker(int rank) {
    char ready[15] = "ALL FILE";
    MPI_Send(ready, 15, MPI_CHAR, TRACKER_RANK, TRACKER_REQUEST_TAG, MPI_COMM_WORLD);
}

void receive_file_info_from_tracker(SegmentSwarm *segments, int *segments_number) {
    if (!segments || !segments_number) {
        fprintf(stderr, "Error: Invalid segments or segments_number pointer.\n");
        return;
    }

    MPI_Recv(segments_number, 1, MPI_INT, TRACKER_RANK, TRACKER_SEGMENT_NUMBER_TAG, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
    if (*segments_number <= 0) {
        fprintf(stderr, "Error: No segments received from tracker.\n");
        return;
    }

    for (int i = 0; i < *segments_number; ++i) {
        MPI_Recv(&segments[i].seedsNumber, 1, MPI_INT, TRACKER_RANK, TRACKER_SEEDS_NUMBER_TAG, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
        MPI_Recv(&segments[i].index, 1, MPI_INT, TRACKER_RANK, TRACKER_INDEX_TAG, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
        MPI_Recv(segments[i].seeds, segments[i].seedsNumber, MPI_INT, TRACKER_RANK, TRACKER_SEEDS_TAG, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
    }
}


void save_info_client_file(int rank, const char *filename, const char *hash, int segment_index) {
    if (filename == NULL || hash == NULL) {
        fprintf(stderr, "Invalid parameters: filename or hash is NULL.\n");
        return;
    }

    char clientfilename[MAX_FILENAME];
    snprintf(clientfilename, MAX_FILENAME, "client%d_%s", rank, filename);

    FILE *file_pointer;
    if (segment_index == 0) {
        file_pointer = fopen(clientfilename, "w");
    } else {
        file_pointer = fopen(clientfilename, "a");
    }

    if (file_pointer == NULL) {
        fprintf(stderr, "Error: Unable to open or create file: %s\n", clientfilename);
        return;
    }

    size_t write_result = fwrite(hash, sizeof(char), HASH_SIZE, file_pointer);
    if (write_result < HASH_SIZE) {
        fprintf(stderr, "Error writing hash to file: %s\n", clientfilename);
        fclose(file_pointer);
        return;
    }
    if (fputs("\n", file_pointer) == EOF) {
        fprintf(stderr, "Error appending newline to file: %s\n", clientfilename);
    }
    fclose(file_pointer);
}

int GetClientByRank(int rank)
{
    for(int i = 0; i < clientsNumber; i++)
    {
        if (clients[i].rank == rank)
        {
            return i;
        }
    }
    return -1;
}

void *download_thread_func(void *arg) {
    int rank = *(int*) arg;
    Client *client = &clients[GetClientByRank(rank)];
    int segments_downloaded_since_update = 0;

    while(client->files_requested_completed < client->desired_files_number) {
        for(int i = 0; i < client->desired_files_number; i++) {
            int segments_count = 0;
            SegmentSwarm segments[MAX_CHUNKS];

            send_request_to_tracker(rank, client->desired_files[i].filename);
            receive_file_info_from_tracker(segments, &segments_count);

            for (int j = 0; j < segments_count; j++) {
                char hash[HASH_SIZE + 1];
                send_segment_request_to_seed(rank, i, segments, client->desired_files[i].filename, j, client);
                receive_segment_from_seed(i, client, j, hash, segments[j].seeds[0]);
                segments_downloaded_since_update++;

                if (segments_downloaded_since_update >= 10) {
                    segments_downloaded_since_update = 0;

                    printf("[Rank %d] Requesting updated seeds/peers list for file '%s'.\n", rank, client->desired_files[i].filename);
                    send_request_to_tracker(rank, client->desired_files[i].filename);
                    receive_file_info_from_tracker(segments, &segments_count);

                    printf("[Rank %d] Resuming download after tracker update.\n", rank);
                }
            }

            if (client->desired_files[i].segmentsReceivedNumber == segments_count) {
                printf("File %s completed\n", client->desired_files[i].filename);
                for (int j = 0; j < client->desired_files[i].segmentsReceivedNumber; j++) {
                    save_info_client_file(rank, client->desired_files[i].filename, client->desired_files[i].segmentsSwarms[j].hash, j);
                }
                client->files_requested_completed++;
            }
        }
    }

    send_ready_request_to_tracker(rank);
    return NULL;
}

void handle_segment_request(MPI_Status *status, Client *client) {
    char desired_filename[MAX_FILENAME] = {0};
    MPI_Recv(desired_filename, sizeof(desired_filename), MPI_CHAR, status->MPI_SOURCE, TRACKER_FILENAME_TAG, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
    
    int desired_segment_index = -1;
    MPI_Recv(&desired_segment_index, 1, MPI_INT, status->MPI_SOURCE, TRACKER_INDEX_TAG, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

    int file_found = 0;
    for (int i = 0; i < client->files_owned_count; i++) {
        if (strcmp(client->files_owned[i].filename, desired_filename) == 0) {
            file_found = 1;
            if (desired_segment_index >= 0 && desired_segment_index < client->files_owned[i].segmentSwarmsNumber) {
                MPI_Send(client->files_owned[i].segmentSwarms[desired_segment_index].hash, HASH_SIZE + 1, MPI_CHAR, status->MPI_SOURCE, CHUNK_TAG, MPI_COMM_WORLD);
                printf("Sent segment %d of file %s to peer %d\n", desired_segment_index, desired_filename, status->MPI_SOURCE);
            } else {
                fprintf(stderr, "Error: Invalid segment index %d requested for file %s\n", desired_segment_index, desired_filename);
            }
            break;
        }
    }
    if (!file_found) {
        fprintf(stderr, "Error: File %s not found in owned files.\n", desired_filename);
    }
}

void *upload_thread_func(void *arg) {
    int rank = *(int*)arg;
    Client *client = &clients[GetClientByRank(rank)];

    while (1) {
        MPI_Status status;
        char request_type[15] = {0};

        MPI_Recv(request_type, sizeof(request_type), MPI_CHAR, MPI_ANY_SOURCE, TRACKER_REQUEST_TAG, MPI_COMM_WORLD, &status);

        if (strncmp(request_type, "req hash", sizeof(request_type)) == 0) {
            handle_segment_request(&status, client);
        } else if (strncmp(request_type, "DONE", sizeof(request_type)) == 0) {
            printf("Received DONE request from peer %d. Exiting upload thread.\n", rank);
            break;
        }
    }

    return NULL;
}

void initialize_segment(SegmentSwarm *segment, const char *segment_hash, int owner, int index) {
    strcpy(segment->hash, segment_hash);
    segment->seedsNumber = 0;
    segment->seeds[segment->seedsNumber++] = owner;
    segment->index = index;
}

void SendAckToAllClients(int numtasks)
{
    for (int client_rank = 1; client_rank < numtasks; client_rank++) 
    { 
        char ack[10] = "ACK";
        printf("[Rank %d] Sending ACK to client %d\n", TRACKER_RANK, client_rank);
        fflush(stdout);
        MPI_Send(ack, 10, MPI_CHAR, client_rank, TRACKER_REQUEST_TAG, MPI_COMM_WORLD);
    }
}

void initializeTracker(int numtasks, int rank) {
    char segmentHash[HASH_SIZE] = {0};
    int i, j, k;
    for(i = 1; i < numtasks; i++) {
        int owned_files_number = 0;
        MPI_Recv(&owned_files_number, 1, MPI_INT, i, TRACKER_OWNED_FILES_NUMBER, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
        printf("[Rank %d] Received owned files count %d from client %d\n", TRACKER_RANK, owned_files_number, i);
        fflush(stdout);

        for(j = 0; j < owned_files_number; j++) {
            char filename[MAX_FILENAME] = {0};
            int segmentsNumber = 0;
            MPI_Recv(filename, MAX_FILENAME, MPI_CHAR, i, TRACKER_FILENAME_TAG, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
            MPI_Recv(&segmentsNumber, 1, MPI_INT, i, TRACKER_SEGMENT_NUMBER_TAG, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

            files[fileSwarmsLength].segmentSwarmsNumber = 0;
            strcpy(files[fileSwarmsLength].filename, filename);

            for(k = 0; k < segmentsNumber; k++) {
                MPI_Recv(segmentHash, HASH_SIZE, MPI_CHAR, i, CHUNK_TAG, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

                SegmentSwarm current_segment;
                initialize_segment(&current_segment, segmentHash, i, k);
                files[fileSwarmsLength].segmentSwarms[files[fileSwarmsLength].segmentSwarmsNumber++] = current_segment;
            }
            fileSwarmsLength++;
        }
    }
    SendAckToAllClients(numtasks);
}

void send_info_to_client(char *filename, MPI_Status *status) {
    int i, j;
    for(i = 0; i < fileSwarmsLength; i++) {
        if (strcmp(files[i].filename, filename) == 0) {
            MPI_Send(&files[i].segmentSwarmsNumber, 1, MPI_INT, status->MPI_SOURCE, TRACKER_SEGMENT_NUMBER_TAG, MPI_COMM_WORLD);

            for(j = 0; j < files[i].segmentSwarmsNumber; j++) {
                MPI_Send(&files[i].segmentSwarms[j].seedsNumber, 1, MPI_INT, status->MPI_SOURCE, TRACKER_SEEDS_NUMBER_TAG, MPI_COMM_WORLD);
                MPI_Send(&files[i].segmentSwarms[j].index, 1, MPI_INT, status->MPI_SOURCE, TRACKER_INDEX_TAG, MPI_COMM_WORLD);
                MPI_Send(files[i].segmentSwarms[j].seeds, files[i].segmentSwarms[j].seedsNumber, MPI_INT, status->MPI_SOURCE, TRACKER_SEEDS_TAG, MPI_COMM_WORLD);
            }
            break;
        }
    }
}

bool allClientsFinished(int clientsNumber, bool *doneClients)
{
    for(int i = 1; i < clientsNumber; i++)
    {
        if(!doneClients[i])
        {
            printf("[Rank %d] Client %d is not done\n", TRACKER_RANK, i);
            fflush(stdout);
            return false;
        }
    }
    printf("[Rank %d] All clients finished\n", TRACKER_RANK);
    fflush(stdout);
    return true;
}


void sendFinishToPeers(int numtasks)
{
    char message[10] = "DONE";
    for(int i = 1; i < numtasks; i++) {
        printf("[Rank %d] Sending finish message to peer %d\n", TRACKER_RANK, i);
        fflush(stdout);
        MPI_Send(message, 10, MPI_CHAR, i, TRACKER_REQUEST_TAG, MPI_COMM_WORLD);
    }
}

void tracker(int numtasks, int rank) {
    char request[15];
    char filename[MAX_FILENAME];

    initializeTracker(numtasks, rank);

    bool doneClients[numtasks];
    for(int i = 0; i < numtasks; i++) {
        doneClients[i] = false;
    }

    while(1) {
        MPI_Status status;
        MPI_Recv(request, 15, MPI_CHAR, MPI_ANY_SOURCE, TRACKER_REQUEST_TAG, MPI_COMM_WORLD, &status);

        if (strcmp(request, "req seeds") == 0) {
            MPI_Recv(filename, MAX_FILENAME, MPI_CHAR, status.MPI_SOURCE, TRACKER_FILENAME_TAG, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
            send_info_to_client(filename, &status);

        } else if(strcmp(request, "ALL FILE") == 0){
            doneClients[status.MPI_SOURCE] = true;
            printf("seed %d is ready\n", status.MPI_SOURCE);

            if (allClientsFinished(numtasks, doneClients) == true)
            {
                printf("[Rank %d] All clients finished, sending finish signal to seeds\n", rank);
                fflush(stdout);
                sendFinishToPeers(numtasks);
                return;
            }
        }
    }
    return;
}

void read_input_file(int rank) {
    Client *client = &clients[GetClientByRank(rank)];
    char inputFileName[MAX_FILENAME];
    char segmentHash[HASH_SIZE];
    snprintf(inputFileName, sizeof(inputFileName), "in%d.txt", rank);

    FILE *inputFile = fopen(inputFileName, "r");

    if (!inputFile) {
        fprintf(stderr, "[Rank %d] Error: Cannot open input file '%s'\n", rank, inputFileName);
        exit(EXIT_FAILURE);
    }

    fscanf(inputFile, "%d", &client->files_owned_count);
    MPI_Send(&client->files_owned_count, 1, MPI_INT, TRACKER_RANK, TRACKER_OWNED_FILES_NUMBER, MPI_COMM_WORLD);

    char filename[MAX_FILENAME];
    int fileSegments;
    int segmentIndex, fileIndex;

    for (fileIndex = 0; fileIndex < client->files_owned_count; fileIndex++) {
        fscanf(inputFile, "%s", filename);
        snprintf(client->files_owned[fileIndex].filename, sizeof(client->files_owned[fileIndex].filename), "%s", filename);
        MPI_Send(filename, MAX_FILENAME, MPI_CHAR, TRACKER_RANK, TRACKER_FILENAME_TAG, MPI_COMM_WORLD);

        fscanf(inputFile, "%d", &fileSegments);
        client->files_owned[fileIndex].segmentSwarmsNumber = fileSegments;
        MPI_Send(&fileSegments, 1, MPI_INT, TRACKER_RANK, TRACKER_SEGMENT_NUMBER_TAG, MPI_COMM_WORLD);

        for (segmentIndex = 0; segmentIndex < fileSegments; segmentIndex++) {
            fscanf(inputFile, "%s", segmentHash);
            strcpy(client->files_owned[fileIndex].segmentSwarms[segmentIndex].hash, segmentHash);
            MPI_Send(segmentHash, HASH_SIZE, MPI_CHAR, TRACKER_RANK, CHUNK_TAG, MPI_COMM_WORLD);
        }
    }

    fscanf(inputFile, "%d", &client->desired_files_number);

    for (int requestIndex = 0; requestIndex < client->desired_files_number; requestIndex++) {
        fscanf(inputFile, "%s", filename);
        strcpy(client->desired_files[requestIndex].filename, filename);

        memset(client->desired_files[requestIndex].segmentsReceived, -1, sizeof(client->desired_files[requestIndex].segmentsReceived));
    }

    fclose(inputFile);
}

void AddClient(int clientRank) {
    clients = (Client *)realloc(clients, (clientsNumber + 1) * sizeof(Client));
    if (clients == NULL) {
        perror("Failed to allocate memory for clients");
        exit(EXIT_FAILURE);
    }

    clients[clientsNumber].rank = clientRank;
    clients[clientsNumber].files_owned_count = 0;
    clients[clientsNumber].desired_files_number = 0;
    clients[clientsNumber].files_requested_completed = 0;

    memset(clients[clientsNumber].files_owned, 0, sizeof(clients[clientsNumber].files_owned));
    memset(clients[clientsNumber].desired_files, 0, sizeof(clients[clientsNumber].desired_files));

    clientsNumber++;
}

void peerInitialize(int rank)
{
    AddClient(rank);
    read_input_file(rank);

    char trackerResponse[10];
    MPI_Recv(trackerResponse, 10, MPI_CHAR, TRACKER_RANK, TRACKER_REQUEST_TAG, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
}

void peer(int numtasks, int rank) {

    peerInitialize(rank);

    pthread_t download_thread;
    pthread_t upload_thread;
    void *status;
    int r;
 
    r = pthread_create(&download_thread, NULL, download_thread_func, (void *) &rank);
    if (r) {
        printf("Eroare la crearea thread-ului de download\n");
        exit(-1);
    }

    r = pthread_create(&upload_thread, NULL, upload_thread_func, (void *) &rank);
    if (r) {
        printf("Eroare la crearea thread-ului de upload\n");
        exit(-1);
    }

    r = pthread_join(download_thread, &status);
    if (r) {
        printf("Eroare la asteptarea thread-ului de download\n");
        exit(-1);
    }

    r = pthread_join(upload_thread, &status);
    if (r) {
        printf("Eroare la asteptarea thread-ului de upload\n");
        exit(-1);
    }
}
 
int main (int argc, char *argv[]) {
    int numtasks, rank;
 
    int provided;
    MPI_Init_thread(&argc, &argv, MPI_THREAD_MULTIPLE, &provided);
    if (provided < MPI_THREAD_MULTIPLE) {
        fprintf(stderr, "MPI nu are suport pentru multi-threading\n");
        exit(-1);
    }
    MPI_Comm_size(MPI_COMM_WORLD, &numtasks);
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);

    if (rank == TRACKER_RANK) {
        tracker(numtasks, rank);
    } else {
        peer(numtasks, rank);
    }

    MPI_Finalize();
}