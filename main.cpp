/****************************************************************************************************************************************************
* File: main.cpp
* Author: Kerby Kaska
*
* Modification History:
* 1/20/2022    Kerby Kaska     Created.
* 1/21/2022    Kerby Kaska     Added "help" and "exit" commands. Added SIGKILL, SIGSTOP, and SIGTERM to invoke signal_handler()
* 1/23/2022    Kerby Kaska     Created close_queues() utility function to clean up queue resources and avoid code redundancy
* 2/01/2022    Kerby Kaska     Added waitpid() to end of server loop to avoid child zombie process 
* 2/05/2022    Kerby Kaska     Added kill_process() to kill opposing client/server process on error to avoid zombies
*
* Procedures:
* main                 - client/server program using message queues to prompt user for commands, receive their results, and print them to the console
*
* close_queues         - convenience method to close both message queue descriptors to avoid code redundancy
*
* kill_process         - utility method to send a SIGKILL to the provided process ID and wait for it to exit
*
* signal_handler       - signal handler for SIGINT, SIGKILL, SIGSTOP, and SIGTERM to ensure proper cleanup of resources used
****************************************************************************************************************************************************/

#include <iostream>
#include <mqueue.h>
#include <unistd.h>
#include <errno.h>
#include <sys/utsname.h>
#include <signal.h>
#include <cstring>
#include <sys/wait.h>

/********************************************************************************************************************************
 * Queue Descriptors:
 * commandQueue         mqd_t                message queue descriptor used to identify the server input message queue
 * responseQueue        mqd_t                message queue descriptor used to identify the server output message queue
 *******************************************************************************************************************************/
static mqd_t commandQueue;
static mqd_t responseQueue;

/********************************************************************************************************************************
 * Queue Constants:
 * COMMAND_QUEUE_NAME       const char*           name of the message queue handled by commandQueue
 * RESPONSE_QUEUE_NAME      const char*           name of the message queue handled by responseQueue
 * QUEUE_MAX_MESSAGES       const unsigned int    maximum number of messages in the queue before blocking new messages
 * QUEUE_MESSAGE_SIZE       const unsigned int    number of bytes indicating the size of an individual queue message
 * QUEUE_PERMISSIONS        const int             octal Unix read/write/execute file permissions granted to the queue on creation
 * QUEUE_MESSAGE_PRIORITY   const int             message priority of all messages sent through the message queue
 *******************************************************************************************************************************/
static const char* COMMAND_QUEUE_NAME           = "/pgm1_mq_command";
static const char* RESPONSE_QUEUE_NAME          = "/pgm1_mq_response";
static const unsigned int QUEUE_MAX_MESSAGES    = 10;
static const unsigned int QUEUE_MESSAGE_SIZE    = 1024;
static const int QUEUE_PERMISSIONS              = 0777; // read/write/execute for everyone
static const int QUEUE_MESSAGE_PRIORITY         = 15;

/********************************************************************************************************************************
 * Command Constants:
 * CMD_GET_DOMAIN_NAME      const char*           command string to be entered by the user for getting the system domain name
 * CMD_GET_HOST_NAME        const char*           command string to be entered by the user for getting the system host name
 * CMD_GET_UNAME            const char*           command string to be entered by the user for getting the system Unix name
 * CMD_GET_HELP             const char*           command string to be entered by the user for getting a useful help message
 * CMD_EXIT                 const char*           command string to be entered by the user for exiting the program
 *******************************************************************************************************************************/
static const char* CMD_GET_DOMAIN_NAME  = "getdomainname";
static const char* CMD_GET_HOST_NAME    = "gethostname";
static const char* CMD_GET_UNAME        = "uname";
static const char* CMD_GET_HELP         = "help";
static const char* CMD_EXIT             = "exit";

/********************************************************************************************************************************
 * Message Constants:
 * MESSAGE_PROMPT           const char*           console message printed to the user when prompted to enter a command
 * MESSAGE_HELP             const char*           message returned to the user as a result of the CMD_GET_HELP command
 * MESSAGE_EXIT             const char*           message returned to the user as a result of the CMD_EXIT command
 * MESSAGE_BAD_COMMAND      const char*           message format used to format the message returned for an unknown command
 * MESSAGE_UNAME            const char*           message format used to format the message returned for CMD_GET_UNAME
 *******************************************************************************************************************************/
static const char* MESSAGE_PROMPT       = "Enter a command: ";
static const char* MESSAGE_HELP         = "Available Commands:\n"
                                          " > getdomainname - get the system domain name and print it to the console\n"
                                          " > gethostname - get the system host name and print it to the console\n"
                                          " > uname - get the system Unix name and print it to the console\n"
                                          " > help - gets this help message and prints it to the console\n"
                                          " > exit - exit the application";
static const char* MESSAGE_EXIT         = "Goodbye!";
static const char* MESSAGE_BAD_COMMAND  = "Unknown command: \"%s\"";
static const char* MESSAGE_UNAME        = " System: %s\n"
                                          "   Node: %s\n"
                                          "Release: %s\n"
                                          "Version: %s\n"
                                          "Machine: %s\n"
                                          " Domain: %s";

/********************************************************************************************************************************
 * static int close_queues(void)
 * Author: Kerby Kaska
 * Date: 1/23/2022
 * Modification History:
 * 1/23/2022    Kerby Kaska     Created.
 *
 * Description: Utility method to close queue descriptors to avoid code redundancy.
 *     Attempts to close the message queue descriptors held by commandQueue and responseQueue. On error, an error message 
 *     is printed to the console and EXIT_FAILURE is returned. On success, EXIT_SUCCESS is returned.
 *
 * Parameters:
 *     close_queues    O/P    int    EXIT_FAILURE on error, EXIT_SUCCESS on success
 *******************************************************************************************************************************/
static int close_queues()
{
    if (mq_close(commandQueue) == -1 || mq_close(responseQueue) == -1) 
    {
        perror("close_queues::mq_close()");
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}

/********************************************************************************************************************************
 * static void kill_process(pid_t)
 * Author: Kerby Kaska
 * Date: 2/05/2022
 * Modification History:
 * 2/05/2022    Kerby Kaska     Created.
 *
 * Description: Utility method to send a SIGKILL to the provided process ID and wait for it to exit. 
 *      Ensures that no zombies are created when either client or server fail and terminate with an error.
 *
 * Parameters:
 *      processID    I/P    pid_t    the process ID of the process to kill 
 *******************************************************************************************************************************/
static void kill_process(pid_t processID)
{
    // send the kill signal
    if (kill(processID, SIGKILL) == -1)
    {
        perror("kill_process::kill()");
    }
    // wait for the process to exit to avoid a zombie
    if (waitpid(processID, NULL, 0) == -1)
    {
        std::cerr << "kill_process::waitpid() - unable to wait for process (" << processID << ") to exit.\n";
    }
}

/********************************************************************************************************************************
 * static void signal_handler(int signalNum)
 * Author: Kerby Kaska
 * Date: 1/20/2022
 * Modification History:
 * 1/20/2022    Kerby Kaska     Created.
 * 1/23/2022    Kerby Kaska     Refactored to call close_queues() instead to clean up resources.
 *
 * Description: Signal handler for SIGINT, SIGKILL, SIGSTOP, and SIGTERM to ensure proper cleanup of resources used.
 *     On error, an error message is printed to the console and the program is terminated EXIT_FAILURE.
 *     On success, the program is terminated EXIT_SUCCESS.
 *
 * Parameters:
 *     signalNum    I/P    int    (unused) indicator of the system signal which triggered the handler
 *******************************************************************************************************************************/
static void signal_handler(int signalNum)
{
    exit(close_queues());
}

/********************************************************************************************************************************
 * int main(int argc, char* argv[])
 * Author: Kerby Kaska
 * Date: 1/20/2022
 * Modification History:
 * 1/20/2022    Kerby Kaska     Created.
 * 1/23/2022    Kerby Kaska     Refactored to call close_queues() instead to clean up resources.
 * 2/01/2022    Kerby Kaska     Added waitpid() to end of server loop to avoid child zombie process 
 * 2/05/2022    Kerby Kaska     Refactored to use kill_process() to ensure no zombies are created on error for either client/server
 * 
 * Description: Main event loop for a a multi-process client/server program using fork that utilizes message queues to transfer 
 *              requests and results. The client process makes requests to the server, waits for a result, and then prints the 
 *              result to the console. The server process listens for incoming requests, executes the proper function based on 
 *              the given command, and returns the result to the client
 *
 * Parameters:
 *     argc    I/P    int      (unused) number of provided command line arguments
 *     argv    I/P    char**   (unused) provided command line arguments 
 *     main    O/P    int      EXIT_SUCCESS on success, EXIT_FAILURE on error
 *******************************************************************************************************************************/
int main(int argc, char* argv[])
{
    char inputBuffer[QUEUE_MESSAGE_SIZE]; // input buffer - commands for server, command responses for client
    char outputBuffer[QUEUE_MESSAGE_SIZE]; // output buffer - command responses for server, commands for client
    
    // register signals to clean up message queues (just in case user hits CTRL+C)
    signal(SIGINT, signal_handler);
    signal(SIGKILL, signal_handler);
    signal(SIGSTOP, signal_handler);
    signal(SIGTERM, signal_handler);
    
    // create and configure the message queue attributes
    mq_attr queueAttributes;
    queueAttributes.mq_maxmsg = QUEUE_MAX_MESSAGES;
    queueAttributes.mq_msgsize = QUEUE_MESSAGE_SIZE;
    queueAttributes.mq_flags = 0; // flag queue to block on mq_send/mq_receive 

    // create or open the message queues for read/write
    commandQueue = mq_open(COMMAND_QUEUE_NAME, O_RDWR | O_CREAT, QUEUE_PERMISSIONS, &queueAttributes);
    if (commandQueue == -1)
    {
        perror("commandQueue::mq_open()");
        return EXIT_FAILURE;
    }
    responseQueue = mq_open(RESPONSE_QUEUE_NAME, O_RDWR | O_CREAT, QUEUE_PERMISSIONS, &queueAttributes);
    if (responseQueue == -1)
    {
        perror("responseQueue::mq_open()");
        if (mq_close(commandQueue) == -1) // close commandQueue (since it opened successfully)
        {
            perror("commandQueue::mq_close()");
        }
        if (mq_unlink(COMMAND_QUEUE_NAME) == -1) // unlink commandQueue (so it is deleted)
        {
            perror("commandQueue::mq_unlink()");
        }
        return EXIT_FAILURE;
    }

    // unlink the message queues, so they are deleted on the system when all descriptors lose reference to it
    if (mq_unlink(COMMAND_QUEUE_NAME) == -1) 
    {
        perror("commandQueue::mq_unlink()");
        close_queues();
        return EXIT_FAILURE;
    }
    if (mq_unlink(RESPONSE_QUEUE_NAME) == -1) 
    {
        perror("responseQueue::mq_unlink()");
        close_queues();
        return EXIT_FAILURE;
    }
    
    const pid_t processID = fork();
    if (processID > 0) // server/parent process
    {
        bool running = true;
        while(running)
        {
            // wait for a command from the client (blocking) and then process it
            // NOTE: On failure, getdomainname, gethostname, and uname copy an error message to the output buffer instead
            if (mq_receive(commandQueue, inputBuffer, sizeof(inputBuffer), NULL) != -1) 
            {
                // getdomainname
                if (strcmp(inputBuffer, CMD_GET_DOMAIN_NAME) == 0)
                {
                    if (getdomainname(outputBuffer, sizeof(outputBuffer)) == -1)
                    {
                        strncpy(outputBuffer, strerror(errno), sizeof(outputBuffer));
                    }
                }
                // gethostname
                else if (strcmp(inputBuffer, CMD_GET_HOST_NAME) == 0)
                {
                    if (gethostname(outputBuffer, sizeof(outputBuffer)) == -1)
                    {
                        strncpy(outputBuffer, strerror(errno), sizeof(outputBuffer));
                    }
                }
                // uname
                else if (strcmp(inputBuffer, CMD_GET_UNAME) == 0)
                {
                    utsname name;
                    if (uname(&name) == -1)
                    {
                        strncpy(outputBuffer, strerror(errno), sizeof(outputBuffer));
                    }
                    else
                    {
                        // format and copy the uname response to the output buffer
                        snprintf(outputBuffer, sizeof(outputBuffer), MESSAGE_UNAME, 
                            name.sysname, name.nodename, name.release, name.version, 
                            name.machine, name.domainname);
                    }
                }
                // exit
                else if (strcmp(inputBuffer, CMD_EXIT) == 0)
                {
                    strncpy(outputBuffer, MESSAGE_EXIT, sizeof(outputBuffer));
                    running = false; // stop looping so the server will exit
                }
                // help
                else if (strcmp(inputBuffer, CMD_GET_HELP) == 0)
                {
                    strncpy(outputBuffer, MESSAGE_HELP, sizeof(outputBuffer));
                }
                // bad command
                else
                {
                    // format and copy invalid command message, including the given message, into output buffer
                    snprintf(outputBuffer, sizeof(outputBuffer) - strlen(MESSAGE_BAD_COMMAND), 
                        MESSAGE_BAD_COMMAND, inputBuffer);
                }

                // send the response back to the child
                if (mq_send(responseQueue, outputBuffer, sizeof(outputBuffer), QUEUE_MESSAGE_PRIORITY) == -1) 
                {
                    perror("server::mq_send()");
                    close_queues();
                    kill_process(processID); // kill client process
                    return EXIT_FAILURE;
                }

                // clear the buffers
                inputBuffer[0] = '\0';
                outputBuffer[0] = '\0';
            }
            else 
            {
                perror("server::mq_receive()");
                close_queues();
                kill_process(processID); // kill client process
                return EXIT_FAILURE;
            }
            
        } // while(running)

        // wait for the child process to exit normally
        // if it fails, manually send the SIGKILL to force terminate it
        if (waitpid(processID, NULL, 0) == -1)
        {
            std::cerr << "server::waitpid() - unable to wait for child process (" << processID << ") to exit.\n";
            kill_process(processID);
        }
    }
    else if (processID == 0) // client/child process
    {
        // print help message and prompt on client start
        std::cout << MESSAGE_HELP << std::endl;
        std::cout << MESSAGE_PROMPT;

        bool running = true;
        std::string input;
        while (running && getline(std::cin, input)) // get console input from user
        {
            strncpy(outputBuffer, input.c_str(), sizeof(outputBuffer));

            // send the command to the parent/server on the commandQueue
            if (mq_send(commandQueue, outputBuffer, sizeof(outputBuffer), QUEUE_MESSAGE_PRIORITY) == -1)
            {
                perror("client::mq_send()");
                close_queues();
                kill_process(getppid()); // kill the server process
                return EXIT_FAILURE;
            }

            // wait for the response to the command on the responseQueue (blocking)
            if (mq_receive(responseQueue, inputBuffer, sizeof(inputBuffer), NULL) != -1)
            {
                std::cout << inputBuffer << std::endl; // print the result to the console
            }
            else
            {
                perror("client::mq_receive()");
                close_queues();
                kill_process(getppid()); // kill the server process
                return EXIT_FAILURE;
            }

            // stop looping if user input "exit" command
            // NOTE: at this point, we sent the "exit" command to the server, which will cause the server loop to exit as well
            if (strcmp(outputBuffer, CMD_EXIT) == 0) 
            {
                running = false;
            }
            else
            {
                std::cout << MESSAGE_PROMPT; // print the prompt again (only if we aren't exiting)
            }

            // clear the buffers
            inputBuffer[0] = '\0';
            outputBuffer[0] = '\0';
        }
    }
    else
    {
        perror("fork()");
        close_queues();
        return EXIT_FAILURE;
    }

    return close_queues();
}