#define _GNU_SOURCE
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

//--------------------------------------------------------------------------------------------
// Constants
//--------------------------------------------------------------------------------------------
#define DEFAULT_PORT 12345
#define MAX_USERS 10
#define MAX_MESSAGE_LENGTH 100
#define MAX_NUM_GUESSES 26
#define NO_CONNECTION -1

//--------------------------------------------------------------------------------------------
// Global variables
//--------------------------------------------------------------------------------------------
int serverfileDescriptor;
pthread_t threads[MAX_USERS];                                           // The thread pool
pthread_mutex_t requestMutex = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;  // RECURSIVE mutex, since a handler thread might try to lock it twice consecutively.
pthread_mutex_t screenMutex = PTHREAD_MUTEX_INITIALIZER;                // Mutex to stop multiple threads writing to the screen at once
pthread_cond_t gotRequestThreadCond = PTHREAD_COND_INITIALIZER;         // Global condition variable
char messageBuffers[MAX_USERS][MAX_MESSAGE_LENGTH];                     // Buffer for each thread
int clientConnections[MAX_USERS];
char *hangmanWordsInUse[MAX_USERS];
char *clientWordsInUse[MAX_USERS];

// Leaderboard critical section related stuff
int leaderboardReadCount = 0;
pthread_mutex_t leaderboardReadCountMutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t leaderboardReadMutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t leaderboardWriteMutex = PTHREAD_MUTEX_INITIALIZER;

// Define a struct to represent a word to be guessed in Hangman,and declare an Array to store them
typedef struct hangmanWordStruct
{
    char *objectName;
    char *objectType;
} hangman_word_t;
hangman_word_t *hangmanWords; // Array of hangman_word_t structs
int numWords;

// Define a struct to represent a User and their login info,and declare an Array to store them
typedef struct UserInfoStruct
{
    char *username;
    char *password;
} user_info_t;
user_info_t *users; // Array of user_info_t structs
int numUsers;
char *loggedInUsers[MAX_USERS];

// Define a struct to represent a client's request, and declare a linked list to store them
typedef struct RequestStruct
{
    int fileDescriptor;             // File descriptor of the client
    struct sockaddr_in addressInfo; // Client's address info
    socklen_t addressSize;          // Client's address size
    struct RequestStruct *next;     // Pointer to the next request
} request_t;
request_t *requests = NULL;    // Head of the linked list of requests
request_t *lastRequest = NULL; // Pointer to the tail of the linked list
int numRequests = 0;

// Define a struct to represent an item on the leaderboard, and declare a linked list to store them
typedef struct LeaderboardItemStruct
{
    char *username;
    int gamesWon;
    int totalGames;
    double percentageWon;
    struct LeaderboardItemStruct *next;
} leaderboard_item_t;
leaderboard_item_t *leaderboardItems = NULL; // Head of the linked list of leaderboard items
int numLeaderboardItems = 0;

//--------------------------------------------------------------------------------------------
// Functions related to making sure we exit gracefully
//--------------------------------------------------------------------------------------------
void close_sockets()
{
    printf("Closing sockets...\n");
    close(serverfileDescriptor);

    // Go through the open connections and close them
    for (int i = 0; i < MAX_USERS; i++)
    {
        if (clientConnections[i] != NO_CONNECTION)
            close(clientConnections[i]);
    }

    // Go through each unhandled request and close its connection
    request_t *currentRequest = requests;
    while (currentRequest != NULL)
    {
        close(currentRequest->fileDescriptor);
        currentRequest = requests->next;
    }
}

void cancel_threads()
{
    printf("Cancelling threads...\n");

    // Wait on each thread to finish safely. 
    // We've already set the serverClosing flag and closed sockets, so they should be wrapping up quickly
    for (int i = 0; i < MAX_USERS; i++)
    {
        pthread_cancel(threads[i]);   
    }
}

void free_memory()
{
    printf("Freeing Memory...\n");

    // Free all words stored in hangmanWords array, and free the array itself
    for (int i = 0; i < numWords; i++)
    {
        free(hangmanWords[i].objectName);
        free(hangmanWords[i].objectType);
    }
    free(hangmanWords);

    // Free all users and passwords stored in users array, and free the array itself
    for (int i = 0; i < numUsers; i++)
    {
        free(users[i].username);
        free(users[i].password);
    }
    free(users);

    // Free up any memory allocated for current connections on threads that are in the process of being cancelled
    for (int i = 0; i < MAX_USERS; i++)
    {
        if (hangmanWordsInUse[i] != NULL) free(hangmanWordsInUse[i]);
        if (clientWordsInUse[i] != NULL) free(clientWordsInUse[i]);
    }

    // Free requests linked list
    while (requests != NULL)
    {
        request_t *temp = requests->next;
        free(requests);
        requests = temp;
    }

    // Free leaderboard linked list
    while (leaderboardItems != NULL)
    {
        leaderboard_item_t *temp = leaderboardItems->next;
        free(leaderboardItems);
        leaderboardItems = temp;
    }
}

void perform_clean_exit(int exitCode)
{
    printf("\n\nClosing Program...\n");

    // Do everything to try and exit as gracefully as possible
    close_sockets();
    cancel_threads();
    free_memory();

    exit(exitCode);
}

void exit_handler(int signum)
{
    // This is specifically to catch the SIGINT (CTRL+C) signal
    if (signum != SIGINT) 
        return; 

    perform_clean_exit(0);
}

//--------------------------------------------------------------------------------------------
// Custom malloc, calloc, and realloc functions to ensure we handle errors properly
//--------------------------------------------------------------------------------------------
void *custom_malloc(size_t size)
{
    void *allocatedPointer = malloc(size);
    if (!allocatedPointer)
    {
        // malloc failed
        fprintf(stderr, "\nERROR: out of memory\n");
        perform_clean_exit(1);
    }

    return allocatedPointer;
}

void *custom_calloc(size_t numberOfMembers, size_t size)
{
    void *allocatedPointer = calloc(numberOfMembers, size);
    if (!allocatedPointer)
    {
        // calloc failed
        fprintf(stderr, "\nERROR: out of memory\n");
        perform_clean_exit(1);
    }

    return allocatedPointer;
}

void *custom_realloc(void* pointer, size_t size)
{
    void *allocatedPointer = realloc(pointer, size);
    if (!allocatedPointer)
    {
        // realloc failed
        fprintf(stderr, "\nERROR: out of memory\n");
        perform_clean_exit(1);
    }

    return allocatedPointer;
}

//--------------------------------------------------------------------------------------------
// Displaying messages related
//--------------------------------------------------------------------------------------------
void thread_fprintf(FILE *stream, int threadId, char *format, va_list args)
{
    // Format the input message
    char formattedMessage[256];
    vsprintf(formattedMessage, format, args);

    // Print it to stdout with the threadId prefixed
    pthread_mutex_lock(&screenMutex);
    fprintf(stream, "Thread %d: %s\n", threadId, formattedMessage);
    fflush(stream);
    pthread_mutex_unlock(&screenMutex);
}

void thread_printf(int threadId, char *format, ...)
{
    va_list args;
    va_start(args, format);
    thread_fprintf(stdout, threadId, format, args);
    va_end(args);
}

void thread_printf_error(int threadId, char *format, ...)
{
    va_list args;
    va_start(args, format);
    thread_fprintf(stderr, threadId, format, args);
    va_end(args);
}

//--------------------------------------------------------------------------------------------
// Reading files related
//--------------------------------------------------------------------------------------------
char **read_text_file(char *fileName, int *numOfLines)
{
    // This function reads a text file line by line into an array of strings (i.e. an array of char arrays/char pointers)
    // Returns said array directly, and the length of the array is returned on the numOfLines int* input
    int lines_allocated = 128;
    int max_line_len = 100;

    // Allocate lines of text
    char **lines = (char **)custom_calloc(lines_allocated, sizeof(char *));
    if (lines == NULL)
    {
        fprintf(stderr, "Out of memory (lines).\n");
        exit(1);
    }

    FILE *fp = fopen(fileName, "r");
    if (fp == NULL)
    {
        fprintf(stderr, "Error opening file (%s).\n", fileName);
        exit(2);
    }

    int lineNum = 0;
    bool endOfFile = false;
    while (!endOfFile)
    {
        // Have we gone over our line allocation?
        if (lineNum >= lines_allocated)
        {
            int new_size;

            // Double our allocation and re-allocate
            new_size = lines_allocated * 2;
            lines = (char **)custom_realloc(lines, sizeof(char *) * new_size);
            lines_allocated = new_size;
        }
        // Allocate space for the next line
        lines[lineNum] = custom_calloc(max_line_len, sizeof(char));

        // Read the line and store it in lines[lineNum]. Returns NULL if reached end of file
        char *readResult = fgets(lines[lineNum], max_line_len - 1, fp);

        // Check if we've reached the end of the file
        if (readResult == NULL)
        {
            // Exit the loop and store how many lines we found
            endOfFile = true;
            *numOfLines = lineNum;
        }
        else if (lines[lineNum][0] != '\n' && lines[lineNum][0] != '\r') // We don't want to store newline characters, so skip them and read the next line
        {
            // Get rid of CR or LF at end of line
            int characterIndex = strlen(lines[lineNum]) - 1; // Initialise to index of last character on the line
            char currentCharacter = lines[lineNum][characterIndex];
            while (characterIndex >= 0 && (currentCharacter == '\n' || currentCharacter == '\r'))
            {
                characterIndex--;
                currentCharacter = lines[lineNum][characterIndex];
            }
            lines[lineNum][characterIndex + 1] = '\0'; // Set '\0' as the last character, which determines the end of a string in C

            lineNum++;
        }
    }

    // Close file
    fclose(fp);

    return lines;
}

void read_hangman_words()
{
    char **words = read_text_file("hangman_text.txt", &numWords);

    hangmanWords = custom_calloc(numWords, sizeof(hangman_word_t));
    if (hangmanWords == NULL)
    {
        fprintf(stderr, "Out of memory (hangmanWords).\n");
        exit(1);
    }

    for (int i = 0; i < numWords; i++)
    {
        // Splits the given string when it sees a comma so that we get the objectName and objectType separated
        char *objectName = strtok(words[i], ",");
        char *objectType = strtok(NULL, ",");

        // Dynamically allocate memory for the objectName and objectType fields and copy the values over
        hangmanWords[i].objectName = custom_calloc(strlen(objectName) + 1, sizeof(char));
        hangmanWords[i].objectType = custom_calloc(strlen(objectType) + 1, sizeof(char));
        strcpy(hangmanWords[i].objectName, objectName);
        strcpy(hangmanWords[i].objectType, objectType);
    }

    // Don't need the directly read text file lines anymore
    for (int i = 0; i < numWords; i++)
        free(words[i]);
    free(words);
}

void read_users()
{
    // Read in the lines, subracting 1 from the total afterwards as the first row is just headings
    char **userLines = read_text_file("Authentication.txt", &numUsers);
    numUsers--;

    users = custom_calloc(numUsers, sizeof(user_info_t));
    if (users == NULL)
    {
        fprintf(stderr, "Out of memory (users).\n");
        exit(1);
    }

    for (int i = 0; i < numUsers; i++)
    {
        // Splits the given string when it sees a tab so that we get the username and password separated
        char *username = strtok(userLines[i + 1], "\t");
        char *password = strtok(NULL, "\t");

        // Need to trim trailing whitespace from the username cause of the way the text file is formatted
        char *end = username + strlen(username) - 1;
        while (end > username && isspace((unsigned char)*end))
            end--;      // Trim trailing space
        *(end + 1) = 0; // Write new null terminator

        // Dynamically allocate memory for the username and password fields and copy the values over
        users[i].username = custom_calloc(strlen(username) + 1, sizeof(char));
        users[i].password = custom_calloc(strlen(password) + 1, sizeof(char));
        strcpy(users[i].username, username);
        strcpy(users[i].password, password);
    }

    // Don't need the directly read text file lines anymore.
    // Remember to use numUsers+1 as we subtracted 1 before but that first line still needs to have memory freed
    for (int i = 0; i < numUsers + 1; i++)
        free(userLines[i]);
    free(userLines);
}

//--------------------------------------------------------------------------------------------
// Sending/Receiving messages related
//--------------------------------------------------------------------------------------------
void send_client_message(int clientfileDescriptor, char *message, int threadId)
{
    int messageLength = strlen(message);
    int sendResult = send(clientfileDescriptor, message, messageLength, 0);
    if (sendResult == -1)
    {
        thread_printf_error(threadId, "Error sending message.");
    }
}

char *receive_client_message(int clientfileDescriptor, int threadId)
{
    int numBytes = recv(clientfileDescriptor, messageBuffers[threadId], MAX_MESSAGE_LENGTH, 0);
    if (numBytes == -1)
    {
        thread_printf_error(threadId, "Error receiving message.");
        return NULL;
    }
    else if (numBytes == 0)
    {
        thread_printf_error(threadId, "Client has closed connection whilst server tried receiving message.");                
        return NULL;
    }

    // Trim the message to its correct size
    messageBuffers[threadId][numBytes] = '\0';
    return messageBuffers[threadId];
}

//--------------------------------------------------------------------------------------------
// Leaderboard related
//--------------------------------------------------------------------------------------------
void read_lock()
{
    // Lock the read count so we don't have multiple readers accidentally screwing it up
	pthread_mutex_lock(&leaderboardReadMutex);
    pthread_mutex_lock(&leaderboardReadCountMutex);
    leaderboardReadCount++;
    
    // If this is the only active reader, lock the leaderboard so we can't write to it
	if (leaderboardReadCount == 1)
        pthread_mutex_lock(&leaderboardWriteMutex);
        
    // Unlock the read count so other readers can come join the fun
	pthread_mutex_unlock(&leaderboardReadCountMutex);
	pthread_mutex_unlock(&leaderboardReadMutex);
}

void read_unlock()
{
    // Lock the read count as we're messing with it again
	pthread_mutex_lock(&leaderboardReadCountMutex);
    leaderboardReadCount--;
    
    // If this is the last active reader, unlock the leaderboard so it can be written to again
	if (leaderboardReadCount == 0)
        pthread_mutex_unlock(&leaderboardWriteMutex);
        
    // We're finished with the read count
	pthread_mutex_unlock(&leaderboardReadCountMutex);
}

void write_lock()
{
    // Can only write if there are no readers reading and no other writers writing
	pthread_mutex_lock(&leaderboardReadMutex);
	pthread_mutex_lock(&leaderboardWriteMutex);
}

void write_unlock()
{
    // Let all other readers and writers have their fun once again
	pthread_mutex_unlock(&leaderboardWriteMutex);
	pthread_mutex_unlock(&leaderboardReadMutex);
}

double get_percentage_won(leaderboard_item_t *item)
{
    double gamesWon = (double)item->gamesWon;
    double totalGames = (double)item->totalGames;
    return gamesWon / totalGames;
}

bool send_leaderboard(int clientfileDescriptor, int threadId)
{
    char messageToSend[MAX_MESSAGE_LENGTH];    
    char* receivedMessage;

    // Lock the leaderboard so no writers can write to it whilst we're reading and stuff
    read_lock();

    // First send the number of items in the leaderboard
    sprintf(messageToSend, "%d", numLeaderboardItems);
    send_client_message(clientfileDescriptor, messageToSend, threadId);
    receivedMessage = receive_client_message(clientfileDescriptor, threadId); // Verify that the client received the message
    if (receivedMessage == NULL) 
    {
        read_unlock();
        return false;
    }

    // Now send each item individually
    leaderboard_item_t *item = leaderboardItems;    
    for (int i = 0; i < numLeaderboardItems; i++)
    {
        sprintf(messageToSend, "%s|%d|%d", item->username, item->gamesWon, item->totalGames);
        send_client_message(clientfileDescriptor, messageToSend, threadId);
        receivedMessage = receive_client_message(clientfileDescriptor, threadId); // Verify that the client received the message
        if (receivedMessage == NULL) 
        {
            read_unlock();
            return false;
        }

        item = item->next;
    }

    // Unlock the leaderboard
    read_unlock();

    // Indicate the server is ready to continue to the main menu
    send_client_message(clientfileDescriptor, "Y", threadId);

    // Return true as we encountered no errors
    return true;
}

int compare_leaderboard_items(leaderboard_item_t *item1, leaderboard_item_t *item2)
{
    // This function works similary to the strcmp function.
    // Returns value < 0 if item1 < item2
    // Returns value = 0 if item1 == item2
    // Returns value > 0 if item1 > item2
    // Determined by, in order of precedence:
    //  - Games won (Ascending)
    //  - Percentage of games won (Ascending)
    //  - Alphabetical order
    if (item1->gamesWon < item2->gamesWon)
    {
        return -1;
    }
    else if (item1->gamesWon == item2->gamesWon)
    {
        if (item1->percentageWon < item2->percentageWon)
        {
            return -1;
        }
        else if (item1->percentageWon == item2->percentageWon)
        {
            return strcmp(item1->username, item2->username);
        }
    }

    return 1;
}

void insert_leaderboard_item_at_correct_pos(leaderboard_item_t *startItem, leaderboard_item_t *newItem)
{
    leaderboard_item_t *currentItem = startItem;
    while (currentItem->next != NULL && compare_leaderboard_items(currentItem->next, newItem) < 0)
    {
        currentItem = currentItem->next;
    }

    newItem->next = currentItem->next;
    currentItem->next = newItem;
}

void add_leaderboard_item(char* currentUser, bool gameWon)
{
    leaderboard_item_t *newItem = custom_malloc(sizeof(leaderboard_item_t));
    newItem->username = currentUser;
    newItem->gamesWon = gameWon ? 1 : 0;
    newItem->totalGames = 1;
    newItem->percentageWon = get_percentage_won(newItem);
    numLeaderboardItems++;    

    if (leaderboardItems == NULL)
    {
        // Leaderboard is empty. Make this the first item
        newItem->next = NULL;
        leaderboardItems = newItem;
        return;
    }

    // Find where to insert this item into the leaderboard.
    if (leaderboardItems == NULL || compare_leaderboard_items(leaderboardItems, newItem) >= 0)
    {
        // The newItem is less than all the current items, or there are no current items, so make it the new head of the linked list
        newItem->next = leaderboardItems;
        leaderboardItems = newItem;
    }
    else
    {
        // Search for where to insert this new item, starting at the head of the linked list
        insert_leaderboard_item_at_correct_pos(leaderboardItems, newItem);
    }
}

void update_leaderboard_item(leaderboard_item_t *previousItem, leaderboard_item_t *item, bool gameWon)
{
    if (gameWon)
    {
        item->gamesWon++;
    }
    item->totalGames++;
    item->percentageWon = get_percentage_won(item);

    // See if this user's position in the leaderboard needs to be updated.
    // If nextItem is NULL, this user is already at the top
    if (item->next != NULL && compare_leaderboard_items(item, item->next) > 0)
    {
        // This item is greater than the next item. Have the previousItem point to the nextItem,
        // then find where the current item is meant to be moved up to.
        if (previousItem == NULL)
        {
            leaderboardItems = item->next;
            previousItem = leaderboardItems;
        }
        else
        {
            previousItem->next = item->next;
        }

        item->next = NULL;

        // Search for where to insert this new item, starting at the previousItem
        insert_leaderboard_item_at_correct_pos(previousItem, item);
    }
}

void update_leaderboard(int threadId, bool gameWon)
{
    // Lock the leaderboard as we don't want multiple threads updating it at once
    write_lock();

    // Try and find the current user in the leaderboard
    char *currentUser = loggedInUsers[threadId];
    bool itemFound = false;
    leaderboard_item_t *previousItem = NULL;
    leaderboard_item_t *item = leaderboardItems;
    while (item != NULL && !itemFound)
    {
        if (strcmp(item->username, currentUser) == 0)
            itemFound = true;
        else
        {
            previousItem = item;
            item = item->next;
        }
    }

    // If the current user isn't already on the leaderboard, add them. Otherwise update their existing item.
    if (item == NULL)
    {
        // Item doesn't exist in the leaderboard, create a new item
        add_leaderboard_item(currentUser, gameWon);
    }
    else
    {
        // Item already exists, update existing item
        update_leaderboard_item(previousItem, item, gameWon);
    }

    // Unlock the leaderboard so other threads can do their thang
    write_unlock();
}

//--------------------------------------------------------------------------------------------
// Running the actual game related
//--------------------------------------------------------------------------------------------
bool is_user_valid(int clientfileDescriptor, int threadId)
{
    char* message;

    // Send message asking for username, receive message for username
    send_client_message(clientfileDescriptor, "\nPlease enter your username: ", threadId);
    message = receive_client_message(clientfileDescriptor, threadId);
    if (message == NULL) return false;
    thread_printf(threadId, "Received username: %s", message);

    // Check username is in users
    for (int i = 0; i < numUsers; i++)
    {
        user_info_t user = users[i];
        if (strcmp(user.username, message) == 0)
        {
            // Send message asking for password, receive message for password
            send_client_message(clientfileDescriptor, "Please enter your password: ", threadId);
            message = receive_client_message(clientfileDescriptor, threadId);
            if (message == NULL) return false;
            thread_printf(threadId, "Received password");

            // Check password is attributed to user
            if (strcmp(user.password, message) == 0)
            {
                loggedInUsers[threadId] = user.username;
                return true;
            }
            else
            {
                return false;
            }
        }
    }

    return false;
}

bool play_hangman(int clientfileDescriptor, int threadId)
{
    thread_printf(threadId, "Client '%s' playing hangman...", loggedInUsers[threadId]);

    // Generate a random number for selecting the hangman words
    int randomNumber = rand() % numWords;
    thread_printf(threadId, "Got random number %d", randomNumber);

    hangman_word_t hangmanWordItem = hangmanWords[randomNumber];
    char *objectType = hangmanWordItem.objectType;
    char *objectName = hangmanWordItem.objectName;
    int hangmanWordLength = strlen(objectType) + 1 + strlen(objectName); // +1 for the space

    // Combine to get a single string with both objectType and objectName separated by a space
    char *hangmanWord = custom_calloc(hangmanWordLength + 1, sizeof(char));
    hangmanWordsInUse[threadId] = hangmanWord; // Save it to ensure if the thread gets cancelled we can free this memory properly
    strcpy(hangmanWord, objectType);
    strcat(hangmanWord, " ");
    strcat(hangmanWord, objectName);
    hangmanWord[hangmanWordLength] = '\0';
    thread_printf(threadId, "Random word chosen: %s", hangmanWord);

    // Determine whether the number of guesses is 26 or the number of characters in both words plus nine
    int numGuesses;    
    if ((hangmanWordLength + 9) > MAX_NUM_GUESSES)
        numGuesses = MAX_NUM_GUESSES;
    else
        numGuesses = (strlen(hangmanWord) + 9);

    thread_printf(threadId, "Number of guesses: %d", numGuesses);

    // Create the initial version of the hangman word to be sent to the client comprised of underscores and a single space
    char *clientWord = custom_calloc(hangmanWordLength + 1, sizeof(char));
    clientWordsInUse[threadId] = clientWord; // Save it to ensure if the thread gets cancelled we can free this memory properly
    for (int i = 0; i < hangmanWordLength; i++)
    {
        clientWord[i] = '_';
    }
    clientWord[strlen(objectType)] = ' ';
    clientWord[hangmanWordLength] = '\0';
    thread_printf(threadId, "Client Word: %s", clientWord);

    int i = 0;
    char guessedLetters[MAX_NUM_GUESSES] = {' '};   
    char messageToSend[MAX_MESSAGE_LENGTH];    
    bool gameWon = false;
    bool gameOver = false; 
    while (!gameOver)
    {
        // Send client guesses made thus far, number of guesses remaining, the current word, and game status
        // Join them all in one message for simplicity
        char gameStatusIndicator = 'O'; // Ongoing
        if (gameWon) 
        {
            gameOver = true;
            gameStatusIndicator = 'W'; // Won
        }
        else if (numGuesses == 0)
        {
            gameOver = true;
            gameStatusIndicator = 'L'; // Lost
        }
        sprintf(messageToSend, "%s|%d|%s|%c", guessedLetters, numGuesses, clientWord, gameStatusIndicator);
        send_client_message(clientfileDescriptor, messageToSend, threadId);

        if (gameStatusIndicator == 'O')
        {
            // Receieve guess from the client
            char *receivedMessage = receive_client_message(clientfileDescriptor, threadId);
            if (receivedMessage == NULL) 
            {
                // Free dynamically allocated memory
                free(hangmanWord);
                free(clientWord);
                return false;
            }

            char guess = receivedMessage[0];
            guessedLetters[i] = guess;
            guessedLetters[i + 1] = '\0';
            numGuesses--;

            // Update the clientWord and check if they've won the game
            gameWon = true;
            for (int j = 0; j < hangmanWordLength; j++)
            {
                // Check if the current character in the array is the same as the guess character
                if (hangmanWord[j] == guess)
                {
                    // Change the client hangman word to have the guessed character in the same position as the original word
                    clientWord[j] = guess;
                } 
                else if (clientWord[j] == '_')
                {
                    // Make the winning boolean false if there is still any underscores found in the client word
                    gameWon = false;
                }
            }

            i++;
        }
    }

    update_leaderboard(threadId, gameWon);

    // Free dynamically allocated memory
    fflush(stdout);
    free(hangmanWord);
    free(clientWord);

    return true;
}

bool main_menu(int clientfileDescriptor, int threadId)
{
    bool quitMenu = false;
    while (!quitMenu)
    {
        thread_printf(threadId, "Client '%s' on main menu...", loggedInUsers[threadId]);

        // Recieve selection for user menu selection
        char *selection = receive_client_message(clientfileDescriptor, threadId);
        if (selection == NULL) return false;
        thread_printf(threadId, "Received selection: %s", selection);

        switch (selection[0])
        {
            case '1':
                // play_hangman() returns false if there is an error, so we should quit the game if that's the case
                quitMenu = !play_hangman(clientfileDescriptor, threadId);
                break;
            case '2':
                // send_leaderboard() returns false if there is an error, so we should quit the game if that's the case
                quitMenu = !send_leaderboard(clientfileDescriptor, threadId);
                break;
            case '3':
                quitMenu = true;
                break;
            default:
                printf("\nInvaild Selection");
                return false;
        }
    }

    // Return true as we finished successfully
    return true;
}

//--------------------------------------------------------------------------------------------
// Handling requests related
//--------------------------------------------------------------------------------------------
void add_request(int fileDescriptor, struct sockaddr_in addressInfo, socklen_t addressSize, pthread_mutex_t *p_mutex, pthread_cond_t *p_cond_var)
{
    // Create new request
    request_t *request = (request_t *)custom_malloc(sizeof(request_t));

    // Setup request
    request->fileDescriptor = fileDescriptor;
    request->addressInfo = addressInfo;
    request->addressSize = addressSize;
    request->next = NULL;

    // Lock the mutex, to assure exclusive access to the linked list of requests
    pthread_mutex_lock(p_mutex);

    // Add new request to the end of the list, updating list pointers as required
    if (numRequests == 0)
    {
        // Special case - list is empty
        requests = request;
        lastRequest = request;
    }
    else
    {
        lastRequest->next = request;
        lastRequest = request;
    }

    // Increase total number of pending requests by one.
    numRequests++;

    // Unlock mutex
    pthread_mutex_unlock(p_mutex);

    // Signal the condition variable - there's a new request to handle
    pthread_cond_signal(p_cond_var);
}

request_t *get_request(pthread_mutex_t *p_mutex)
{
    // lock the mutex, to assure exclusive access to the list
    pthread_mutex_lock(p_mutex);

    request_t *request;
    if (numRequests > 0)
    {
        request = requests;
        requests = request->next;
        if (requests == NULL)
        {
            // This was the last request on the list
            lastRequest = NULL;
        }
        // decrease the total number of pending requests
        numRequests--;
    }
    else
    {
        // requests list is empty
        request = NULL;
    }

    // Unlock mutex and return the request to the caller.
    pthread_mutex_unlock(p_mutex);
    return request;
}

void handle_request(int clientfileDescriptor, int threadId)
{
    // Authenticate user
    if (!is_user_valid(clientfileDescriptor, threadId))
    {
        thread_printf_error(threadId, "User failed to validate");
        send_client_message(clientfileDescriptor, "false", threadId);
        return;
    }

    // Notify the client they've logged in successfully
    send_client_message(clientfileDescriptor, "true", threadId);
    thread_printf(threadId, "User '%s' successfully authenticated", loggedInUsers[threadId]);

    if (!main_menu(clientfileDescriptor, threadId))
    {
        thread_printf_error(threadId, "Error playing hangman");
        return;
    }
}

void handle_requests_loop(void *data)
{
    int threadId = *((int *)data);
    thread_printf(threadId, "CREATED");

    // Lock the mutex, to access the requests list exclusively.
    pthread_mutex_lock(&requestMutex);

    // Infinite loop where we continuously check to see if a request is pending, and handle it if so
    while (1)
    {
        // Check if a request is pending
        if (numRequests > 0)
        {
            // If we get a request - handle it and free it
            request_t *request = get_request(&requestMutex);
            if (request)
            {
                // Unlock mutex so other threads would be able to handle other requests waiting in the queue paralelly.
                pthread_mutex_unlock(&requestMutex);

                // The only thing we really need is the file descriptor, so get that and free the memory allocated for the request
                int clientfileDescriptor = request->fileDescriptor;
                clientConnections[threadId] = clientfileDescriptor;
                free(request);

                // Deal with the client
                thread_printf(threadId, "STARTED handling request for %s", inet_ntoa(request->addressInfo.sin_addr));
                handle_request(clientfileDescriptor, threadId);
                
                thread_printf(threadId, "Finished handling request for %s", inet_ntoa(request->addressInfo.sin_addr));
                close(clientfileDescriptor);
                clientConnections[threadId] = NO_CONNECTION;                
                loggedInUsers[threadId] = NULL;

                // Lock the mutex again, we want to check the numRequests variable to see if there are any requests
                pthread_mutex_lock(&requestMutex);
            }
        }
        else
        {
            // Wait for a request to arrive. Note the mutex will be unlocked here, thus allowing other threads access
            // to the requests list.
            // Passing requestMutex to this function means that after we return from pthread_cond_wait, the mutex
            // is locked again, so we don't need to lock it ourselves
            pthread_cond_wait(&gotRequestThreadCond, &requestMutex);
        }
    }
}

//--------------------------------------------------------------------------------------------
// main
//--------------------------------------------------------------------------------------------
int main(int argc, char **argv)
{
    // Check they're running the program correctly
    if (argc > 2)
    {
        fprintf(stderr, "usage: Server port\n");
        exit(1);
    }

    int port = DEFAULT_PORT;
    if (argc == 2)
    {
        // Get port number from command line arguments
        port = atoi(argv[1]);
        if (port <= 0)
        {
            fprintf(stderr, "Please specify a valid port number\n");
            exit(1);
        }
    }

    // Seed the random number generator
    srand(time(NULL));

    // Set exit_handler() to trigger when a SIGINT signal is received (i.e. when Ctrl+C is pressed)
    if (signal(SIGINT, exit_handler) == SIG_ERR)
        printf("\nCan't catch SIGINT\n");

    // Read and store the words we'll be using for Hangman, as well as the info of the Users that are allowed to connect
    read_hangman_words();
    read_users();

    // Generate a socket for the server
    serverfileDescriptor = socket(AF_INET, SOCK_STREAM, 0);
    if (serverfileDescriptor == -1)
    {
        perror("socket");
        exit(1);
    }

    // Generate the end point (my address info)
    struct sockaddr_in serveraddressInfo;
    serveraddressInfo.sin_family = AF_INET;         // host byte order
    serveraddressInfo.sin_port = htons(port);       // short, network byte order
    serveraddressInfo.sin_addr.s_addr = INADDR_ANY; // auto-fill with my IP

    // Bind the socket to the end point
    int bindResult = bind(serverfileDescriptor, (struct sockaddr *)&serveraddressInfo, sizeof(struct sockaddr));
    if (bindResult == -1)
    {
        perror("bind");
        exit(1);
    }

    // Start listening on the created socket
    int listenResult = listen(serverfileDescriptor, MAX_USERS);
    if (listenResult == -1)
    {
        perror("listen");
        exit(1);
    }

    // Create threads to handle incoming client requests
    int threadIds[MAX_USERS];
    for (int i = 0; i < MAX_USERS; i++)
    {
        threadIds[i] = i;
        clientConnections[i] = NO_CONNECTION;
        pthread_create(&threads[i], NULL, (void *(*)(void *))handle_requests_loop, (void *)&threadIds[i]);
    }

    // Infinite loop to handle client connections
    while (1)
    {
        struct sockaddr_in clientaddressInfo;                     // Client's address info
        socklen_t clientaddressSize = sizeof(struct sockaddr_in); // Need to initialise this to be the size of the struct for clientaddressInfo

        // Accept any incoming connection
        int clientfileDescriptor = accept(serverfileDescriptor, (struct sockaddr *)&clientaddressInfo, &clientaddressSize);
        if (clientfileDescriptor == -1)
        {
            perror("accept");
            continue;
        }

        // Do whatever with the connection
        printf("server: got connection from %s\n", inet_ntoa(clientaddressInfo.sin_addr));
        add_request(clientfileDescriptor, clientaddressInfo, clientaddressSize, &requestMutex, &gotRequestThreadCond);
    }

    return 0;
}
