#include "process.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>

#define errorExit(status)  perror("pipe"), exit(status)
#define errorSingleExit(name, status)  perror(name), exit(status)
#define STACK_INIT_SIZE 4

int zombies = 0; //track the number of "zombie" background processes, shouldn't wait for those in the wait loops

//stack for directories
typedef struct _charStack {
	int size;
	int capacity;
	char** elements;
} charStack;

charStack* directoryStack = NULL; //Keep track of directory stack

//Report Error
void errorStatus(char* message, bool extract) {
	int error = errno;
	perror(message); //Report it
	char buffer[4];
	sprintf(buffer, "%d", (extract? STATUS(error) : error)); //Convert error to exit status
	setenv("?", buffer, 1); //Set exit status
}

void redirectFile(const CMD *cmdList) {
	int redirect = -1;
	if (cmdList->fromType == RED_IN) {
		redirect = open(cmdList->fromFile, O_RDONLY);
		if (redirect < 0) { //Error
			int error = errno; //If redirect failed, store the error number
			errorSingleExit (cmdList->argv[0], error); //Report the error with perror, do not execute command - exit the child process with the error number exit code (will be reaped by parent)
		}

		dup2(redirect, 0);
		close(redirect);
	}
	else if (cmdList->fromType == RED_IN_HERE) {
		char template[] = "XXXXXX";
		redirect = mkstemp(template);
		if (redirect < 0) { //Error
			int error = errno; //If mkstemp failed, store the error number
			errorSingleExit (cmdList->argv[0], error); //Report the error with perror, do not execute command - exit the child process with the error number exit code (will be reaped by parent)
		}
		unlink(template);
		write(redirect, (void*) cmdList->fromFile, strlen(cmdList->fromFile));
		lseek(redirect,0,SEEK_SET);
		dup2(redirect, 0);
		close(redirect);
	}
	if (cmdList->toType == RED_OUT) {
		redirect = open(cmdList->toFile, O_WRONLY | O_CREAT | O_TRUNC, 00666);
		if (redirect < 0) { //Error
			int error = errno; //If redirect failed, store the error number
			errorSingleExit (cmdList->argv[0], error); //Report the error with perror, do not execute command - exit the child process with the error number exit code (will be reaped by parent)
		}
		dup2(redirect, 1);
		close(redirect);
	}
	else if (cmdList->toType == RED_OUT_APP) {
		redirect = open(cmdList->toFile, O_WRONLY | O_CREAT | O_APPEND, 00666);
		if (redirect < 0) { //Error
			int error = errno; //If redirect failed, store the error number
			errorSingleExit (cmdList->argv[0], error); //Report the error with perror, do not execute command - exit the child process with the error number exit code (will be reaped by parent)
		}
		dup2(redirect, 1);
		close(redirect);
	}
}

void executeSingle(const CMD *cmdList) {

	//Execute command with redirection
	int pid = fork();

	if (pid < 0) { //Error - fork failed from parent
		errorStatus("fork", false);
		return;
	}

	//Child code
	else if (pid == 0) {
		//Add local variables to environment for function
		for (int i = 0; i < cmdList->nLocal; i++) {
			// printf("%s: %s\n", cmdList->locVar[i], cmdList->locVal[i]);
			setenv(cmdList->locVar[i], cmdList->locVal[i], 1);
		}
		redirectFile(cmdList);
		execvp(cmdList->argv[0], cmdList->argv);
		int error = errno; //If execvp failed, store the error number
		errorSingleExit (cmdList->argv[0], error); //Report the error with perror, exit the process with the error number as the exit code
	}

	//Parent code
	else {
		int result = -1;
		waitpid(pid, &result, 0); //Collect the exit status of the child process in result
		if (result != -1) { //If we actually collected the status from here (rather than it being reaped elsewhere - eg, SIGINT interrupt handler), then set status
			char buffer[4];
			sprintf(buffer, "%d", STATUS(result)); //Convert the exit status to status, and set the environment variable
			setenv("?", buffer, 1);
		}		
	}
}

//In order traversal to flatten tree of pipes
void countPipes(const CMD* cmdList, int* size) {
	if (cmdList->type != PIPE) {
		(*size)++;
		return;
	}
	else {
		countPipes(cmdList->left, size);
		countPipes(cmdList->right, size);
	}
}

//In order traversal to flatten tree of pipes
void flattenPipes(const CMD* cmdList, const CMD*** pipeList, int* i) {
	if (cmdList->type != PIPE) {
		(*pipeList)[*i] = cmdList;
		(*i)++;
		return;
	}
	else {
		flattenPipes(cmdList->left, pipeList, i);
		flattenPipes(cmdList->right, pipeList, i);
	}
}

void executePipe(const CMD *cmdList) {

	//First, flatten out a tree of multiple pipes into a list of commands
	//Instead of creating dynamic array, just traversing tree one more time to figure out how big the array needs to be
	int size = 0;
	countPipes(cmdList, &size);

	const CMD** pipeList = malloc(sizeof(CMD*) * size); //NEED TO FREE!
	int x = 0;
	flattenPipes(cmdList, &pipeList, &x);
	//pipeList now contains an ordered list of commands in the multiple pipes, from left to right

	/* printf("Processing pipes:\n");
	for (int i = 0; i < size; i++) {
		printf("%s %s\n", pipeList[i]->argv[0], pipeList[i]->argv[1]);
	} */

	//Creating the file descriptors for pipe
	int fd[2],                  // Read and write file descriptors for pipe
	pid, result,            // Process ID and status of child
	fdin,                   // Read end of last pipe (or original stdin)
	i;
	int processes[size];

    fdin = 0;                                   // Remember original stdin
    for (i = 0; i < size-1; i++) {              // Create chain of processes
		if (pipe(fd) == -1) {
			errorStatus("pipe: pipe faild", false);
			return;
		}
		else if ((pid = fork()) < 0) {
			errorStatus("fork", false);
			return;
		}

		else if (pid == 0) {                    // Child process
			close (fd[0]);                      //  No reading from new pipe

			if (fdin != 0)  {                   //  stdin = read[last pipe]
				dup2 (fdin, 0);
				close (fdin);
			}

			if (fd[1] != 1) {                   //  stdout = write[new pipe]
				dup2 (fd[1], 1);
				close (fd[1]);
			}
			if (i == 0) {
				//Add local variables to environment only for first pipe function
				for (int j = 0; j < pipeList[i]->nLocal; j++) {
					// printf("%s: %s\n", cmdList->locVar[i], cmdList->locVal[i]);
					setenv(pipeList[i]->locVar[j], pipeList[i]->locVal[j], 1);
				}
			}
			redirectFile(pipeList[i]);

			if (pipeList[i]->type == SIMPLE) {
				execvp (pipeList[i]->argv[0], pipeList[i]->argv);
				int error = errno; //If execvp failed, store the error number
				errorExit (error); //Print error message, exit the child program with the error number (wait loop at end will catch this while reaping)
			}

			else if (pipeList[i]->type == SUBCMD) {
				for (int j = 0; j < cmdList->nLocal; j++) {
					// printf("%s: %s\n", cmdList->locVar[i], cmdList->locVal[i]);
					setenv(pipeList[i]->locVar[j], pipeList[i]->locVal[j], 1);
				}
				process(pipeList[i]->left); //The actual commands in the subcommand
				exit(atoi(getenv("?"))); //Exit with the status of the last executed command
			}
			
		} 
		
		else {                                // Parent process
			processes[i] = pid;	//track pid of child process			
			if (i > 1) {                         //   Close read[last pipe]
				close (fdin);                   //    if not original stdin
			}

			fdin = fd[0];                       //  Remember read[new pipe]
			close (fd[1]);                      //  No writing to new pipe
		}
    }

    if ((pid = fork()) < 0)  {                   // Create last process
		errorStatus("fork", false);
		return;
	}

    else if (pid == 0) {                        // Child process
		if (fdin != 0) {                        //  stdin = read[last pipe]
			dup2 (fdin, 0);
			close (fdin);
		}
		redirectFile(pipeList[size-1]);

		if (pipeList[size-1]->type == SIMPLE) {
			execvp (pipeList[size-1]->argv[0], pipeList[size-1]->argv);
			int error = errno; //If execvp failed, store the error number
			errorExit (error); //Print error message, exit the program with the error number (wait loop at end will catch this while reaping)
		}
		
		else if (pipeList[size-1]->type == SUBCMD) {
			for (int j = 0; j < pipeList[size-1]->nLocal; j++) {
				// printf("%s: %s\n", cmdList->locVar[i], cmdList->locVal[i]);
				setenv(pipeList[size-1]->locVar[j], pipeList[size-1]->locVal[j], 1);
			}
			process(pipeList[size-1]->left); //The actual commands in the subcommand
			exit(atoi(getenv("?"))); //Exit with the status of the last executed command
		}
		errorExit (EXIT_FAILURE);
    } 
	
	else {                                    // Parent process
		processes[size-1] = pid;	//track pid of last child process
		if (i > 1) {                             //  Close read[last pipe]
			close (fdin);                       //   if not original stdin
		}
	}
	
	int status = 0;
    for (i = 0; i < size; i++) {                   // Wait for children to die
		pid = wait (&result); //here, we are waiting for any pid to reap, not just the ones in the pipe. Might catch a zombie here
		//Check if the reaped pid is background zombie or pipe - note this is currently inefficient (O (n^2))
		if (pid != -1) { //No error in collecting PID
			for (int j = 0; j < size + 1; j++) {
				if (j >= size) { //Zombie process, since the reaped pid does not exist in the proccesses array
					fprintf(stderr, "Completed: %d (%d)\n", pid, status); //Reaped a zombie
					zombies--;
					i--; //Since this is not a pipe command, need to still reap all pipe commands (so iterate one more time to ignore zombie)
				}
				else if (processes[j] == pid) { //Reaping one of the pipe childs, not a zombie
					//printf("Result: %d\n", result);
					if (STATUS(result) != 0) {
						status = STATUS(result);
					}
					char buffer[4];
					sprintf(buffer, "%d", status);
					setenv("?", buffer, 1);
					break;
				}
			}
		}
		
    }
	
}

void executeConditional(const CMD* cmdList) {
	//Process left subchild first
	CMD* left = cmdList->left;
	process(left);
	char* result = getenv("?");
	
	//Switch based on && or ||
	if (cmdList->type == SEP_AND) {
		if (strcmp(result, "0") == 0) {
			process(cmdList->right);
		}
	}

	else if (cmdList->type == SEP_OR) {
		if (strcmp(result, "0") != 0) {
			process(cmdList->right);
		}
	}

}

void executeSubcommand(const CMD* cmdList) {
	//Fork off a "subshell"
	int pid = fork();

	if (pid < 0) { //Error
		errorStatus("subshell: fork failed", false);
		return;
	}

	//Child code - the subshell
	else if (pid == 0) {
		//Add local variables to environment for the subshell
		for (int i = 0; i < cmdList->nLocal; i++) {
			// printf("%s: %s\n", cmdList->locVar[i], cmdList->locVal[i]);
			setenv(cmdList->locVar[i], cmdList->locVal[i], 1);
		}
		redirectFile(cmdList);
		process(cmdList->left); //The actual commands in the subcommand
		exit(atoi(getenv("?"))); //Exit with the status of the last executed command
	}

	//Parent code
	else {
		int result = -1;
		waitpid(pid, &result, 0);
		if (result != -1) { //If we actually collected the status from here (rather than it being reaped elsewhere - eg, SIGINT interrupt handler), then set status
			char buffer[4];
			sprintf(buffer, "%d", STATUS(result));
			setenv("?", buffer, 1);
		}
	}
}

//In order traversal to flatten tree of pipes
void flattenBG(const CMD* cmdList, const CMD*** backgroundList, CMD** foreground, int* i) {
	if (cmdList->type != SEP_BG && cmdList->type != SEP_END) { //If the node is NOT a ; or &, add it to the background list
		(*backgroundList)[*i] = cmdList;
		(*i)++;
		return;
	}
	
	//If this is a ; node, the left child goes in the foreground, right child goes in the background
	else if (cmdList->type == SEP_END) {
		*foreground = cmdList->left;
		(*backgroundList)[*i] = cmdList->right;
		(*i)++;
	}
	
	//If this is a & node, recurse down the left child, then recurse down the right child
	else if (cmdList->type == SEP_BG) {
		flattenBG(cmdList->left, backgroundList, foreground, i);
		if (cmdList->right != NULL) {
			flattenBG(cmdList->right, backgroundList, foreground, i);
			//SHOULD NOT VISIT THE RIGHT TREE OF THE TOP SEP_BG NODE!
		}
		
	}
}

//In order traversal to flatten tree of pipes
void countBG(const CMD* cmdList, int* i) {
	if (cmdList->type != SEP_BG && cmdList->type != SEP_END) { //If the node is NOT a ; or &, add it to the background list
		(*i)++;
		return;
	}
	
	//If this is a ; node, the left child goes in the foreground, right child goes in the background
	else if (cmdList->type == SEP_END) {
		(*i)++;
	}
	
	//If this is a & node, recurse down the left child, then recurse down the right child
	else if (cmdList->type == SEP_BG) {
		countBG(cmdList->left, i);
		if (cmdList->right != NULL) {
			countBG(cmdList->right, i);
		}
		
	}
}

void executeBackground(const CMD* cmdList) {
	//Stage 1: Extract which commands should be in the foreground, and which should be in the background

	int size = 0; //count how many background commands there are
	const CMD** backgroundList; //Set up a list of all commands to be executed in the background
	CMD* foreground = NULL; //Keep track of any commands that have to be executed in the foreground

	//Only make this list if there is more than one bg / sep node (not if there is only one)
	if (cmdList->left->type == SEP_END || cmdList->left->type == SEP_BG) {
		countBG(cmdList->left, &size); //count how many background commands there are
		backgroundList = malloc(sizeof(CMD*) * size); //malloc enough space in the list
		int j = 0;
		flattenBG(cmdList->left, &backgroundList, &foreground, &j); //populate the list. Start with left subchild since we don't want the algorithm to visit the right child of the root & (since that goes in FG)
		//NOTE, it is safe to start from the left child of the root &. Because, we know that this subchild must either be an & or a ;. 
		//If it is &, the algorithm will visit both left and right. If it is ;, the algorithm will but left in foreground and right in background
	}
	//If there is only one root BG & node, add the left subchild as the only element in the list
	else {
		backgroundList = malloc(sizeof(CMD*));
		backgroundList[0] = cmdList->left;
		size = 1;
	}
	
	//Stage 2: Process all the commands in order
	//Process the command in the foreground
	if (foreground != NULL) {
		process(foreground);
	}

	//Iterate through list of background commands and fork a subchild for each
	for (int i = 0; i < size; i++) {
		int pid = fork();

		if (pid < 0) { //Error
			errorStatus("background, fork failed", false);
			return;
		}

		//Child code - the subshell (background)
		else if (pid == 0) {
			process(backgroundList[i]); //The actual commands in the background (the left node)
			exit(atoi(getenv("?"))); //Exit with the status of the last executed command
		}

		else { //Parent code
			//Don't wait but track the pid
			fprintf(stderr, "Backgrounded: %d\n", pid);
			zombies++;
		}
	}	

	//Parent
	//Process the right hand side of the SEP BG (if it has one)
	if (cmdList->right != NULL) {
		process(cmdList->right);
	}
	setenv("?", "0", 1); //Set status in parent foreground shell to 0
}

void executeCD (const CMD* cmdList) { //Not tested with pipelines, conditionals, redirection, subcommand. Not tested for edge cases
	int status = 1;
	if (cmdList->argv[1] != NULL) { //Directory specified
		if (cmdList->argv[2] != NULL) { //2 arguments specified
			fprintf(stderr, "usage: cd OR cd <dirName>\n");
			setenv("?", "1", 1); //set exit status to 1
			return;
		}
		if (cmdList->argv[1][0] == '/') {
			status = chdir(cmdList->argv[1]);
			if (status == -1) { //System call failed
				errorStatus("cd: chdir fail", false);
				return;
			}
			else {
				setenv("?", "0", 1); //set exit status to 0
				return;
			}
		}
		else if (cmdList->argv[1][0] == '.' && strlen(cmdList->argv[1]) == 1) { //cd .
			setenv("?", "0", 1); //set exit status to 0
			return;
		}
		else if (cmdList->argv[1][0] == '.' && cmdList->argv[1][1] == '/') {
			char pwd[PATH_MAX];
			char* temp = getcwd(pwd, sizeof(pwd));
			if (temp == NULL) { //System call failed
				errorStatus("cd: getcwd fail", false);
				return;
			}
			//char* pwd = get_current_dir_name(); //NEED TO FREE
			int len = strlen(pwd);
			if (pwd[len - 1] != '/') {
				len++;
			}
			len += strlen(cmdList->argv[1]) - 2; //remove 2 for the ./
			len++; //For null character
			char directory[len];
			memset(directory, 0, len);
			strcat(directory, pwd);
			if (pwd[strlen(pwd) - 1] != '/') {
				strcat(directory, "/");
			}
			strcat(directory, cmdList->argv[1] + 2);
			
			status = chdir(directory);
			if (status == -1) { //System call failed
				errorStatus("cd: chdir fail", false);
				return;
			}
			else {
				setenv("?", "0", 1); //set exit status to 0
				return;
			}
		}
		else if (cmdList->argv[1][0] == '.' && cmdList->argv[1][1] == '.') {
			char pwd[PATH_MAX];
			char* temp = getcwd(pwd, sizeof(pwd));
			if (temp == NULL) { //System call failed
				errorStatus("cd: getcwd fail", false);
				return;
			}
			int pwdLen = strlen(pwd);

			//printf("Getcwd: %s\n", temp);
			
			int parentLen = 0; //How many characters at end of pwd to cut off for the parent directory
			if (pwd[pwdLen - 1] == '/') {
				parentLen++;
			}

			while (pwd[pwdLen - 1 - parentLen] != '/') {
				parentLen++;
			}
			parentLen++; //to account for the last /

			char cutPwd[pwdLen - parentLen + 1];
			strncpy(cutPwd, pwd, pwdLen-parentLen);
			cutPwd[pwdLen-parentLen] = 0;

			//printf("Parent directory: %s\n", cutPwd);

			int len = strlen(cutPwd);
			len += strlen(cmdList->argv[1]) - 2; //remove 2 for the ..
			len += 1; //For null character
			char directory[len];
			memset(directory, 0, len);
			strcat(directory, cutPwd); //Parent directory
			if (strlen(cmdList->argv[1]) > 2) {
				strcat(directory, cmdList->argv[1] + 2); //Whatever was after the ..
			}
			status = chdir(directory);
			if (status == -1) {
				errorStatus("cd: chdir fail", false);
				return;
			}
			else {
				setenv("?", "0", 1); //set exit status to 0
				return;
			}
		}
		else {
			char pwd[PATH_MAX];
			char* temp = getcwd(pwd, sizeof(pwd));
			if (temp == NULL) { //System call failed
				errorStatus("cd: getcwd fail", false);
				return;
			}
			int len = strlen(pwd);
			if (pwd[len - 1] != '/') {
				len++;
			}
			len += strlen(cmdList->argv[1]);
			len++; //For null character
			char directory[len];
			memset(directory, 0, len);
			strcat(directory, pwd);
			if (pwd[strlen(pwd) - 1] != '/') {
				strcat(directory, "/");
			}
			strcat(directory, cmdList->argv[1]);

			status = chdir(directory);
			if (status == -1) { //System call failed
				errorStatus("cd: chdir fail", false);
				return;
			}
			else {
				setenv("?", "0", 1); //set exit status to 0
				return;
			}
		}
	}
	else { //Cd to HOME directory
		status = chdir(getenv("HOME"));
		if (status == -1) { //System call failed
			errorStatus("cd: chdir fail", false);
			return;
		}
		else {
			setenv("?", "0", 1); //set exit status to 0
			return;
		}
	}
	
}

void executePushd(const CMD* cmdList) {
	if (cmdList->argv[2] != NULL) {
		fprintf(stderr, "usage: pushd <dirName>\n");
		setenv("?", "1", 1); //set exit status to 1
		return;
	}
	if (directoryStack == NULL) { //first time we are pushing to stack
		directoryStack = malloc(sizeof(charStack)); //NEED TO FREE!
		directoryStack->size = 0;
		directoryStack->capacity = STACK_INIT_SIZE;
		directoryStack->elements = malloc(sizeof(char*) * STACK_INIT_SIZE);
	}

	if (directoryStack->size >= directoryStack->capacity) {
		directoryStack->capacity *= 2;
		directoryStack->elements = realloc(directoryStack->elements, sizeof(char*) * directoryStack->capacity);
	}

	char pwd[PATH_MAX];
	char* temp = getcwd(pwd, sizeof(pwd));
	if (temp == NULL) {
		errorSingleExit("getcwd", EXIT_FAILURE);
	}
	directoryStack->elements[directoryStack->size] = malloc(sizeof(char) * (strlen(pwd) + 1)); //NEED TO FREE
	strcpy(directoryStack->elements[directoryStack->size], pwd);
	(directoryStack->size)++;

	executeCD(cmdList);

	if (strcmp("0", getenv("?")) == 0) {
		temp = getcwd(pwd, sizeof(pwd));
		if (temp == NULL) {
			errorStatus("cd: getcwd fail", false);
			return;
		}
		else {
				setenv("?", "0", 1); //Set exit status
			}
		printf("%s", pwd);
		for (int i = directoryStack->size - 1; i >= 0; i--) {
			printf(" %s", directoryStack->elements[i]);
		}
		printf("\n");
	}

	else {
		(directoryStack->size)--;
		free(directoryStack->elements[directoryStack->size]);
	}

}

void executePopd(const CMD* cmdList) {
	if (cmdList->argv[1] != NULL) {
		fprintf(stderr, "usage: popd\n");
		setenv("?", "1", 1); //set exit status to 1
		return;
	}
	if (directoryStack == NULL) {
		fprintf(stderr, "popd: dir stack empty\n");
		setenv("?", "1", 1); //Set exit status
	}
	else if (directoryStack->size == 0) {
		fprintf(stderr, "popd: dir stack empty\n");
		setenv("?", "1", 1); //Set exit status
	}
	else {
		(directoryStack->size)--;
		char* temp = cmdList->argv[1];
		free(temp);
		cmdList->argv[1] = directoryStack->elements[directoryStack->size];

		//printf("Calling CD with 2nd argument: %s\n", cmdList->argv[1]);
		executeCD(cmdList);
		
		char pwd[PATH_MAX];
		temp = getcwd(pwd, sizeof(pwd));
		if (temp == NULL) {
			errorStatus("cd: getcwd fail", false);
			return;
		}
		else {
			setenv("?", "0", 1); //Set exit status
		}
		printf("%s", pwd);
		for (int i = directoryStack->size - 1; i >= 0; i--) {
			printf(" %s", directoryStack->elements[i]);
		}
		printf("\n");
	}
}

//Handler for sig int (CTRL-C)
void terminationHandler(int signum) {
	int status = -1;
	wait(&status); //Reap the child process that was terminated

	char buffer[4];
	sprintf(buffer, "%d", STATUS(status)); //Convert interrupted kill of child to exit status
	setenv("?", buffer, 1); //Set exit status to interrupted

	if (status == -1) {
		printf("\n");
	}
}

int process (const CMD *cmdList) {
	//printf("Process called by %d on %s %s\n", getpid(), cmdList->argv[0], cmdList->argv[1]);
	
	//Handling for CTRL-C (SIGINT)
	struct sigaction catchInterrupt;
	catchInterrupt.sa_handler = terminationHandler;
	sigaction(SIGINT, &catchInterrupt, NULL);
	
	//Reap Zombies once per execution
	int status = -1;
	int pid = 0;
	pid = waitpid(-1, &status, WNOHANG);
	while (pid != (pid_t) 0 && pid != -1 && status != -1) {
		fprintf(stderr, "Completed: %d (%d)\n", pid, status); //Reaped a zombie
		zombies--;
		pid = waitpid(-1, &status, WNOHANG);
	}

	//Simple command
	if (cmdList->type == SIMPLE) {
		if (strcmp(cmdList->argv[0], "cd") == 0) {
			executeCD(cmdList);
		}
		else if (strcmp(cmdList->argv[0], "pushd") == 0) {
			executePushd(cmdList);
		}
		else if (strcmp(cmdList->argv[0], "popd") == 0) {
			executePopd(cmdList);
		}
		else {
			executeSingle(cmdList);
		}
	}

	//Pipe
	else if (cmdList->type == PIPE) {
		executePipe(cmdList);
	}

	//Conditional - note, this does not actually fork off children, it calls process again on the left/right node based on exit status
	else if (cmdList->type == SEP_AND || cmdList->type == SEP_OR) {
		executeConditional(cmdList);
	}

	//Subcommand
	else if (cmdList->type == SUBCMD) {
		executeSubcommand(cmdList);
	}

	//Sep end: doesn't fork off children, calls process sequentially on left and right (regardless of exit status)
	else if (cmdList->type == SEP_END) {
		process(cmdList->left);
		process(cmdList->right);
	}

	//Background
	else if (cmdList->type == SEP_BG) {
		//Idea: use a "subshell" to do stuff in the background!
		//Don't have to wait for this subshell, but want to reap it at some point
		executeBackground(cmdList);
	}
	else {
		printf("Not implemented!\n");
	}
	//printf("%d returning\n", getpid());
	return 0;
}
