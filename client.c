#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#define MAX_MESSAGE_LENGTH 1000

//--------------------------------------------------------------------------------------------
// Global variables
//--------------------------------------------------------------------------------------------
int serverFileDescriptor;
char messageBuffer[MAX_MESSAGE_LENGTH];
char* currentUser;

//--------------------------------------------------------------------------------------------
// Functions related to making sure we exit gracefully
//--------------------------------------------------------------------------------------------
void perform_clean_exit(int exitCode)
{
    printf("\n\nClosing Program...\n");
    
    // Close connection to server
    close(serverFileDescriptor);

    // Free dynamically allocated memory
    free(currentUser);

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
// Sending/Receiving messages related
//--------------------------------------------------------------------------------------------
char* get_user_input()
{
    scanf("%s", messageBuffer);
    return messageBuffer;    
}

void send_server_message(int serverFileDescriptor, char* message)
{
    int messageLength = strlen(message);
    int sendResult = send(serverFileDescriptor, message, messageLength, 0);
    if (sendResult == -1)
    {
        fprintf(stderr, "Error sending message.\n");
    }
}

char* receive_server_message(int serverFileDescriptor)
{
    int numBytes = recv(serverFileDescriptor, messageBuffer, MAX_MESSAGE_LENGTH, 0);
    if (numBytes == -1) 
    {
        fprintf(stderr, "Error sending message.\n");
        return NULL;
    }
    else if (numBytes == 0)
    {
        fprintf(stderr, "Server has closed connection whilst client tried receiving message.\n");
        return NULL;
    }

    // Trim the message to its correct size
    messageBuffer[numBytes] = '\0';
    return messageBuffer;
}

//--------------------------------------------------------------------------------------------
// Leaderboard related
//--------------------------------------------------------------------------------------------
bool display_leaderboard(int serverFileDescriptor)
{
    char* receivedMessage;

    // First receive the number of items in the leaderboard
    receivedMessage = receive_server_message(serverFileDescriptor);
    if (receivedMessage == NULL) return false;
    send_server_message(serverFileDescriptor, "Y");    
    int numLeaderboardItems = atoi(receivedMessage);

    if (numLeaderboardItems == 0)
    {
        printf("\n");
        printf("====================================================================\n");
        printf("\n");
        printf("There is no information currently stored in the Leader Board. Try again later\n");
        printf("\n");
        printf("====================================================================\n");
    }
    else
    {
        // Now receive each item individually
        for (int i = 0; i < numLeaderboardItems; i++)
        {
            printf("\n");
            printf("====================================================================\n");
            printf("\n");

            // Receive all details for the single leaderboard item in one go and split it up for simplicity
            receivedMessage = receive_server_message(serverFileDescriptor);
            if (receivedMessage == NULL) return false;
            send_server_message(serverFileDescriptor, "Y");  
            char *username = strtok(receivedMessage, "|");
            int gamesWon = atoi(strtok(NULL, "|"));
            int totalGames = atoi(strtok(NULL, "|"));

            printf("Player - %s\n", username);        
            printf("Number of games won - %d\n", gamesWon);        
            printf("Number of games played - %d\n", totalGames);        
            
            printf("\n");
            printf("====================================================================\n");
        }
    }
    
    // Check the server is ready to continue to the main menu
    receivedMessage = receive_server_message(serverFileDescriptor);
    if (receivedMessage == NULL) return false;

    return true;
}

//--------------------------------------------------------------------------------------------
// Running the actual game related
//--------------------------------------------------------------------------------------------
bool authenticate_user(int serverFileDescriptor)
{
    printf("You are required to logon with your Username and Password\n");
    
    char* message;

    message = receive_server_message(serverFileDescriptor);
    if (message == NULL) return false;
    printf("%s", message);

    // Read the username and send to server
    char* username = get_user_input();    
    currentUser = malloc(strlen(username) + 1);
    if (!currentUser)
    {
        // malloc failed
        fprintf(stderr, "\nERROR: out of memory\n");
        perform_clean_exit(1);
    }
    strcpy(currentUser, username);
    send_server_message(serverFileDescriptor, username);

    // Check if username was correct
    message = receive_server_message(serverFileDescriptor);
    if (message == NULL) return false;
    if (strcmp("false", message) == 0) 
    {
        fprintf(stderr, "\nYou entered an incorrect username - disconnecting\n");
        return false;
    }
    printf("%s", message);
    
    // Read the username and send to server
    char* password = get_user_input();
    send_server_message(serverFileDescriptor, password);

    // Check if password was correct
    message = receive_server_message(serverFileDescriptor);
    if (message == NULL) return false;
    if (strcmp("false", message) == 0) 
    {
        fprintf(stderr, "\nYou entered an incorrect username - disconnecting\n");
        return false;
    }

    return true;
}

bool play_hangman(int serverFileDescriptor)
{
    bool gameFinished = false;
    char* message;
    char* gameFinishedMessage;

    while (!gameFinished)
    {   
        printf("\n--------------------------------------------------------------------\n");
        
        // Receive currently made guesses from server, remaining number of guesses, and the current word from the server
        // They're all joined together as one message for simplicity so we need to separate them with strtok()
        message = receive_server_message(serverFileDescriptor);
        if (message == NULL) return false;    
        char *guessedLetters = strtok(message, "|");
        char *numGuessesString = strtok(NULL, "|");
        char *clientWord = strtok(NULL, "|");
        char *gameStatusIndicator = strtok(NULL, "|");
        
        // Print out the info we received
        printf("\nGuessed letters: %s\n", guessedLetters);
        printf("\nNumber of guesses left: %s\n", numGuessesString);
        printf("\nWord: %s\n", clientWord);        

        // Check the current status of the game
        if (strcmp(gameStatusIndicator, "W") == 0) 
        {
            // Game won
            gameFinishedMessage = "\nWell done %s! You won this round of Hangman!\n";
            gameFinished = true;
        }
        else if (strcmp(gameStatusIndicator, "L") == 0)
        {
            // Game lost
            gameFinishedMessage = "\nBad luck %s! You have run out of guesses. The hangman got you!\n";
            gameFinished = true;
        }
        else
        {
            // Get the next guess from the user
            printf("\nEnter your guess: ");
            message = get_user_input();        
            send_server_message(serverFileDescriptor, message);    
        }
    }

    // Print the end game messages
    printf("\nGame over\n");
    printf("\n");
    printf(gameFinishedMessage, currentUser);        
    printf("Updating leaderboard...\n");
    printf("\n--------------------------------------------------------------------\n");    

    return true;
}

bool main_menu(int serverFileDescriptor)
{
    printf("\n====================================================================\n\n");
    printf("Welcome to the Online Hangman Gaming System\n\n");
    printf("====================================================================\n\n\n");

    if (!authenticate_user(serverFileDescriptor))
        return false;

    printf("\n\n--------------------------------------------------------------------\n\n");
    printf("\nWelcome to the Hangman Gaming System\n");

    bool quitMenu = false;
    while (!quitMenu)
    {
        printf("\n");
        printf("Please enter a selection\n");
        printf("<1> Play Hangman\n");
        printf("<2> Show Leaderboard\n");
        printf("<3> Quit\n\n");

        // Loop the user making a selection, ensuring the user selects one of the three and can try again if they make an error
        char* selectionString;
        char selection;
        bool selectionIsValid = false;
        while (!selectionIsValid)
        {
            printf("Select Option 1 - 3: ");
            selectionString = get_user_input();  
            selection = selectionString[0];
            if (selection == '1' || selection == '2' || selection == '3')
                selectionIsValid = true;
            else
                printf("\nIncorrect Selection\nPlease ");
        }

        // Send the server the selectction and begin playing the corresponding action
        send_server_message(serverFileDescriptor, selectionString);
        switch(selection) 
        {
            case '1':
                quitMenu = !play_hangman(serverFileDescriptor);
                break;
            case '2':
                quitMenu = !display_leaderboard(serverFileDescriptor);
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
// main
//--------------------------------------------------------------------------------------------
int main(int argc, char **argv)
{
    // Check we got an IP and port number input
    if (argc != 3)
    {
        fprintf(stderr, "usage: Client IP and port\n");
        exit(1);
    }

    // Get port number and IP from command line arguments
    char *ipAddress = argv[1];
    int port = atoi(argv[2]);
    if (port == 0)
    {
        fprintf(stderr, "Please specify a valid port number\n");
        exit(1);
    }

    // Set exit_handler() to trigger when a SIGINT signal is received (i.e. when Ctrl+C is pressed)
    if (signal(SIGINT, exit_handler) == SIG_ERR)
        printf("\nCan't catch SIGINT\n");

    // Get the host info
    struct hostent *hostEntity = gethostbyname(ipAddress);
    if (hostEntity == NULL)
    {
        // Apparently herror is obsolete because gethostbyname is obsolete. Compiler throws an annoying warning about an implicit declaration
        //herror("gethostbyname");
        perror("gethostbyname");
        exit(1);
    }

    // Create a socket for connecting to the server
    serverFileDescriptor = socket(AF_INET, SOCK_STREAM, 0);
    if (serverFileDescriptor == -1)
    {
        perror("socket");
        exit(1);
    }

    // Create server's address info for connecting
    struct sockaddr_in serverAddressInfo;
    serverAddressInfo.sin_family = AF_INET;   // host byte order
    serverAddressInfo.sin_port = htons(port); // short, network byte order
    serverAddressInfo.sin_addr = *((struct in_addr *)hostEntity->h_addr_list[0]);
    bzero(&(serverAddressInfo.sin_zero), 8); // zero the rest of the struct

    // Connect to the server
    int connectResult = connect(serverFileDescriptor, (struct sockaddr *)&serverAddressInfo, sizeof(struct sockaddr));
    if (connectResult == -1)
    {
        perror("connect");
        exit(1);
    }

    printf("Connected to server %s:%d\n", inet_ntoa(serverAddressInfo.sin_addr), port);

    bool gameResult = main_menu(serverFileDescriptor);
    if (!gameResult)
        fprintf(stderr, "\nError occurred whilst playing Hangman. Exiting...\n");
    else
        printf("\nExiting Hangman\n");
    
    close(serverFileDescriptor);
    free(currentUser);

    return 0;
}
