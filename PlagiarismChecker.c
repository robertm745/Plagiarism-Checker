#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <dirent.h>
#include <string.h>
#include <pthread.h>
#include <fcntl.h>
#include <errno.h>
#include <ctype.h>
#include <math.h>

// token struct, containing string for word or file path, double for count or probability, next token, and next file
typedef struct t {
	char* word;
	double count;
	struct t* nextTkn;
	struct t* nextFile;
} token_t;

// argument struct, containing file path, pointer to list of tokens of all currently processed files, and mutex
typedef struct s {
	char* path;
	token_t** tknList;
	pthread_mutex_t *mutex;
} args_t;

// utility in checking delimiters for processing files
int isDelim(char c) {
	return c == ' ' || c == '\n' || c == '\0';
}

// tokenizing function to process input string and current index; returns malloc'd word of next token starting from main index, or returns NULL if char at index mainIndex is a delimiter 
char* checkWord(char str[], int mainIndex) {
	if (isDelim(str[mainIndex])) {
		return NULL;
	}
	str[mainIndex] = tolower(str[mainIndex]);
	int nextIndex = mainIndex + 1;
	char nextChar = str[nextIndex];
	// traverse up string until find next delimiter
	while (!isDelim(nextChar)) {
		str[nextIndex] = tolower(str[nextIndex]);
		nextChar = str[++nextIndex]; 
	}
	// allocated space for new word
	char *word = (char*) malloc(sizeof(char) * nextIndex-mainIndex + 1);
	if (word == NULL) {
		puts("Out of memory");
		return NULL;
	}
	// copy word from input string into return word
	word = memcpy(word, str + mainIndex, nextIndex - mainIndex);
	word[nextIndex - mainIndex] = '\0';
	return word;
}

void* myopenfile(void* voidArg) {
	// cast input void* as argument struct, extract file path, and attempt to open file path
	args_t* arg = (args_t*) voidArg;
	char* path = arg->path;
	int fd = open(path, O_RDONLY);
	if (fd == -1) {
		perror("Error opening file");
		puts(path);
		return arg;
	}
	// create new token for newly opened file and initialize struct members accordingly
	token_t *fileTkn = (token_t*) malloc(sizeof(token_t));
	if (!fileTkn) {
		puts("Out of memory");
		return arg;
	}	
	fileTkn->word = (char*) malloc(strlen(path) + 1);
	if (!fileTkn->word) {
		puts("Out of memory");
		return arg;
	}	
	strcpy(fileTkn->word, path);
	fileTkn->count = 0;
	fileTkn->nextTkn = NULL;
	fileTkn->nextFile = NULL;

	// critical section: traverse up tknList until at end of list, then append this file's token struct to the end of the list
	pthread_mutex_lock(arg->mutex);
	token_t* ptr = *(arg->tknList);
	if (ptr == NULL) {
		*(arg->tknList) = fileTkn;
	}
	else {
		while (ptr->nextFile != NULL) {
			ptr = ptr->nextFile;
		}
		ptr->nextFile = fileTkn;
	} 
	pthread_mutex_unlock(arg->mutex);
	// end critical section

	//retrieve number of bytes of file, i.e. file size
	int len = lseek(fd, 0, SEEK_END);
	// reset file pointer to beginning of file
	lseek(fd, 0, SEEK_SET);
	// check for lseek error
	if (len == -1 || len == 0 || len == 1) {
		close(fd);
		if (len == -1) {
			perror("lseek error");
			return arg;
		} else {
			// empty file, nothing to read
			return NULL;
		}
	}

	// extract all file chars into buffer
	char buf[len];
	if (!read(fd, buf, len - 1)) {
		perror("Error reading file into buffer");
		close(fd);
		return arg;
	}
	buf[len - 1] = '\0';

	//begin tokenizing file
	int mainIndex = 0;
	char* nextWord;
	token_t *newTkn, *currTkn, *prevTkn;
	//while mainIndex is not at the end of file...
	while (mainIndex != len) {
		// check for next token
		nextWord = checkWord(buf, mainIndex);
		// if returned word is NULL, not a valid token starting at mainIndex
		if (nextWord) {
			// increment count of total tokens in file and update mainIndex
			fileTkn->count++;
			mainIndex += strlen(nextWord) - 1;
			// set pointers to beginning of tokens of current file to begin insertion into correct locaton within the linked list of tokens for this file
			currTkn = fileTkn->nextTkn;
			prevTkn = NULL;
			while (currTkn) {
				// if new word alphabetically preceedes current token, increment current token
				if (strcmp(currTkn->word, nextWord) < 0) {
					prevTkn = currTkn;
					currTkn = currTkn->nextTkn;
					continue;
				// if duplicate, increment count of word and free duplicated word 
				} else if (strcmp(currTkn->word, nextWord) == 0) {
					currTkn->count++;
					free(nextWord);
					nextWord = NULL;
				} 
				break;
			}
			// if new word was duplicate, continue tokenizing next word
			// if new word was not a duplicate, create new struct to be inserted into current position
			if (nextWord) {
				newTkn = (token_t*) malloc(sizeof(token_t));
				if (!newTkn) {
					puts("Out of memory");
					return arg;
				}	
				newTkn->word = nextWord;
				newTkn->count = 1;
				// check if first token in list
				if (!prevTkn) { 
					fileTkn->nextTkn = newTkn;
				} else {
					prevTkn->nextTkn = newTkn;
				}
				newTkn->nextTkn = currTkn;
			}
		}
		// go to index of next char
		mainIndex++;
	}

	int total = fileTkn->count;
	currTkn = fileTkn->nextTkn;
	// set the count of each token within the file as its probability of occurence within the file
	while (currTkn) {
		currTkn->count = currTkn->count/total;
		currTkn = currTkn->nextTkn;
	}

	// close file
	close(fd);
	return NULL;
}


// function for processing directories
void* myopendir(void* arg) {
	// cast input void* as argument struct and extract path
	args_t* currentDirArgs = (args_t*) arg;
	char* path = currentDirArgs->path;

	// attempt to open directory
	DIR *dir = opendir(path);
	if (!dir) {
		printf("Invalid directory. Path is: %s\n", path);
		return arg;
	}

	// attempt to read first entry
	struct dirent *dp = readdir(dir);
	if (!dp) {
		perror("Error on initial call to readdir");
		closedir(dir);
		return arg;
	}

	// traverse through the current directory and count the number of subdirectories and files
	int totalEntries = 0;
	while (dp) {
		if (dp->d_type == DT_REG || (dp->d_type == DT_DIR && (strcmp(dp->d_name, ".") && strcmp(dp->d_name, "..")))) {
			totalEntries++;
		}
		dp = readdir(dir);
	}
	closedir(dir);

	// create two arrays for argument structs and thread id's, each of computed size
	args_t* args[totalEntries];
	pthread_t threads[totalEntries];

	// reopen dir and begin processing
	dir = opendir(path);
	dp = readdir(dir);
	int index = 0, len;

	// while current entry is not NULL...
	while (dp) {
		// check the type of directory entry for file or subdirectory 
		if (dp->d_type == DT_REG || ((dp->d_type == DT_DIR) && (strcmp(dp->d_name, ".") && strcmp(dp->d_name, "..")))) {

			// initialize new struct for aguments for newpath
			args[index] = (args_t*) malloc(sizeof(args_t));
			if (!args[index]) {
				puts("Out of memory");
				while (index-- > 0) {
					free((args[index])->path);
					free(args[index]);
				}
				closedir(dir);
				return arg;
			}

			// create new string with total length: current path + 1 (for '/') and new path + 1 (for null terminator)
			len = (strlen(path) + 1 + strlen(dp->d_name) + 1);
			args[index]->path = (char*) malloc(len); 
			if (!args[index]->path) {
				puts("Out of memory");
				while (index-- > 0) {
					free((args[index])->path);
					free(args[index]);
				}
				closedir(dir);
				return arg;
			}
			// construct new string with current path + / + current entry's path
			strncpy(args[index]->path, path, strlen(path));
			(args[index]->path)[strlen(path)] = '/';
			strncpy(args[index]->path + strlen(path) + 1, dp->d_name, strlen(dp->d_name));
			(args[index]->path)[len -1] = '\0';

			args[index]->mutex = currentDirArgs->mutex;
			args[index]->tknList = currentDirArgs->tknList;

			// create new thread for file or subdirectory as appropriate and check for error
			if (dp->d_type == DT_REG) {
				if (pthread_create(threads + index, NULL, myopenfile, args[index])) {
					perror("Thread error");
					while (index-- > 0) {
						free((args[index])->path);
						free(args[index]);
					}
					closedir(dir);
					return arg;
				}
			} else { 
				if (pthread_create(threads + index, NULL, myopendir, args[index])) {
					perror("Thread error");
					while (index-- > 0) {
						free((args[index])->path);
						free(args[index]);
					}
					closedir(dir);
					return arg;
				}
			}
			// increment index in thread id's and argument structs array
			index++;
		}
		// read next entry
		dp = readdir(dir);
	}

	// join all threads in thread's array
	void* retval = NULL;
	while (index-- > 0) {
		pthread_join(threads[index], &retval);
		if (retval)
			return arg;
	}

	// free all memory used in current directory's argument structs array 
	while (totalEntries-- > 0) {
		free((args[totalEntries])->path);
		free(args[totalEntries]);
	}

	// close directory
	closedir(dir);
	return NULL;
}

// utility to swap all members of two token structs
void swap(token_t* a, token_t* b) {
	char* tempStr = a->word;
	double tempCount = a->count;
	token_t* tempList = a->nextTkn;
	a->word = b->word;
	a->count = b->count;
	a->nextTkn = b->nextTkn;
	b->word = tempStr;
	b->count = tempCount;
	b->nextTkn = tempList;
}

// utility to print out final results; Note: coloring/conio.h was not working on iLabs for me
void printValues(token_t** arg) {
	if (!arg) 
		return;
	token_t* ptrA = *arg;
	char* red = "RED", *yellow = "YELLOW", *green = "GREEN", *cyan = "CYAN", *blue = "BLUE", *white = "WHITE";
	char *color;
	while (ptrA) {
		if (ptrA->count <= 0.1) { 
			color = red;
		} else if (ptrA->count <= 0.15) { 
			color = yellow;
		} else if (ptrA->count <= 0.2) {
			color = green;
		} else if (ptrA->count <= 0.25) {
			color = cyan;
		} else if (ptrA->count <= 0.3) {
			color = blue;
		} else {
			color = white;
		}

		printf("%s %f %s\n", color, ptrA->count, ptrA->word);
		ptrA = ptrA->nextFile;
	}
}

// utility to print memory of all token within inputted token list
void printMem(token_t** arg) {
	if (!arg) 
		return;
	token_t* ptrA = *arg, *ptrB;
	while (ptrA) {
		printf("Filename: %s\nTotal count is %f\n\n", ptrA->word, ptrA->count);
		ptrB = ptrA->nextTkn;
		while (ptrB) { 
			printf("Word: %s\nCount: %f\n\n", ptrB->word, ptrB->count);
			ptrB = ptrB->nextTkn;
		}
		puts("\n\n");
		ptrA = ptrA->nextFile;
	}
}

// utility to free all memory within a token list
void freeMem(token_t** arg) {
	if (!arg) 
		return;
	token_t* ptrA = *arg, *ptrB, *ptrC;
	while (ptrA) {
		ptrB = ptrA->nextTkn;
		while (ptrB != NULL) {
			ptrC = ptrB->nextTkn;
			free(ptrB->word);
			free(ptrB);
			ptrB = ptrC;
		}
		ptrB = ptrA->nextFile;
		free(ptrA->word);
		free(ptrA);
		ptrA = ptrB;
	}
}

// utility to sort a token list via selection sort
void sortList(token_t** list) {
	if (!list) 
		return;
	token_t* ptrA = *list, *ptrB;
	while (ptrA->nextFile) {
		ptrB = ptrA->nextFile;
		while (ptrB) {
			if (ptrA->count > ptrB->count) {
				swap(ptrA, ptrB);
			}
			ptrB = ptrB->nextFile;
		}
		ptrA = ptrA->nextFile;
	}
}

// main function
int main(int argc, char *argv[]){
	// check for incorrect input
	if (argc != 2 || argv[1] == NULL || strlen(argv[1]) < 1) {
		printf("Required: Two arguments with nonempty directory target\n");
		return 1;
	}

	// check for extra '/' appended to inputted string
	char* path = argv[1];
	if (path[strlen(argv[1]) - 1] == '/') 
		path[strlen(argv[1]) - 1] = '\0';

	// allocate for argument struct for inputted directory as initialize accordingly
	args_t *mainArg = (args_t*) malloc(sizeof(args_t));
	if (!mainArg) {
		puts("Out of memory");
		return 1;
	}

	mainArg->path = path;
	pthread_mutex_t mut = PTHREAD_MUTEX_INITIALIZER;
	mainArg->mutex = &mut;

	mainArg->tknList = (token_t**) malloc(sizeof(token_t*));
	if (!mainArg->tknList) {
		puts("Out of memory");
		pthread_mutex_destroy(mainArg->mutex);
		free(mainArg);
		return 1;
	}
	*(mainArg->tknList) = NULL; 

	// create new thread to start the file analysis process
	pthread_t mainThread;
	if (pthread_create(&mainThread, NULL, myopendir, mainArg)) {
		perror("Main thread error");
		pthread_mutex_destroy(mainArg->mutex);
		freeMem(mainArg->tknList);
		free(mainArg->tknList);
		free(mainArg);
		return 1;
	}
	// wait for all threads to finish before processing data
	void*  retval = NULL;
	pthread_join(mainThread, &retval);
	if (retval) {	
		puts("Error in processing. Exiting...");
		pthread_mutex_destroy(mainArg->mutex);
		freeMem(mainArg->tknList);
		free(mainArg->tknList);
		free(mainArg);
		return 1;
	}

	token_t* ptrA = *(mainArg->tknList);
	token_t* ptrB, *ptrC;

	// check if no file or only one file was found in inputted directory
	if (ptrA == NULL || ptrA->nextFile == NULL) {
		printf("Input directory contains less than two files\n\n");
	} else {
		// sort list of files by total number of tokens
		sortList(mainArg->tknList);
		// printMem(mainArg->tknList);

		// begin processing all pairs of files for similarity analysis
		// create new token list for all sub-lists with one sub-list for each unique pair of files
		token_t *firstMean = (token_t*) malloc(sizeof(token_t));
		if (!firstMean) {
			puts("Out of memory");
			pthread_mutex_destroy(mainArg->mutex);
			freeMem(mainArg->tknList);
			free(mainArg->tknList);
			free(mainArg);
			return 1;
		}
		token_t** meanList = &firstMean;
		ptrA = *(mainArg->tknList);
		token_t* ptrE = firstMean, *ptrD = ptrE, *ptrF, *ptrG;
		int countForA, countForB;
		// method: for each file, compare it with all subsequent files
		// eliminates possibility for redundant pairings
		// while unique file pairs remain...
		while (ptrA->nextFile) {
			ptrB = ptrA->nextFile;
			while (ptrB) {
				// save total count of tokens for files A and B
				countForA = ptrA->count;
				ptrA->count = 0;
				countForB = ptrB->count;
				ptrB->count = 0;

				// create new token and set its 'word' member as the paths of the two current files and initializte its other members accordingly
				ptrE->word = (char*) malloc(strlen(ptrA->word) + strlen(ptrB->word) + 10);
				ptrE->word[0] = '\"';
				ptrE->word[1] = '\0';
				ptrE->word = strcat(ptrE->word, ptrA->word);
				ptrE->word = strcat(ptrE->word, "\" and \"");
				ptrE->word = strcat(ptrE->word, ptrB->word);
				ptrE->word[strlen(ptrA->word) + strlen(ptrB->word) + 8] = '\"';
				ptrE->word[strlen(ptrA->word) + strlen(ptrB->word) + 9] = '\0';
				ptrE->count = 0;
				ptrE->nextTkn = NULL;
				ptrE->nextFile = NULL;
				// update pointers in mean-structure; ptrD = current column, ptrE = current row in ptrD
				ptrD = ptrE;
				// initialize temporary pointers to traverse the two token lists of the current two files
				ptrF = ptrA->nextTkn;
				ptrG = ptrB->nextTkn;
				// while both files have remaining tokens...
				while (ptrF && ptrG) {
					// new temp struct to hold info of next token to be processed
					ptrC = (token_t*) malloc(sizeof(token_t));
					if (!ptrC) {	
						puts("Out of memory");
						freeMem(meanList);
						pthread_mutex_destroy(mainArg->mutex);
						freeMem(mainArg->tknList);
						free(mainArg->tknList);
						free(mainArg);
						return 1;
					}
					// compare the current tokens of the two respective files for alphabetical order
					if (strcmp(ptrF->word, ptrG->word) > 0) {
						//copy word from current token
						ptrC->word = (char*) malloc(strlen(ptrG->word) + 1);
						ptrC->word = strcpy(ptrC->word, ptrG->word);
						// set probability of new token in mean struct accordingly
						ptrC->count = ptrG->count / 2;
						// sum next KLD in current file
						ptrB->count += ptrG->count * log10(ptrG->count / ptrC->count);
						// increment next token
						ptrG = ptrG->nextTkn;
					} else if (strcmp(ptrF->word, ptrG->word) < 0) {
						// same as above but for the other file
						ptrC->word = (char*) malloc(strlen(ptrF->word) + 1);
						ptrC->word = strcpy(ptrC->word, ptrF->word);
						ptrC->count = ptrF->count / 2;
						ptrA->count += ptrF->count * log10(ptrF->count / ptrC->count);
						ptrF = ptrF->nextTkn;
					} else {
						// same as above but for both files as the two current tokens are equal
						ptrC->word = (char*) malloc(strlen(ptrF->word) + 1);
						ptrC->word = strcpy(ptrC->word, ptrF->word);
						ptrC->count = (ptrF->count + ptrG->count)/ 2;
						ptrA->count += ptrF->count * log10(ptrF->count / ptrC->count);
						ptrB->count += ptrG->count * log10(ptrG->count / ptrC->count);
						ptrF = ptrF->nextTkn;
						ptrG = ptrG->nextTkn;
					}
					ptrC->nextTkn = NULL;
					// append the temp token to the end of the current mean list
					ptrE->nextTkn = ptrC;
					ptrE = ptrE->nextTkn;
				}
				// append any remaining tokens in first file's token list to end of current mean list
				while (ptrF) {
					ptrC = (token_t*) malloc(sizeof(token_t));
					if (!ptrC) {	
						puts("Out of memory");
						freeMem(meanList);
						pthread_mutex_destroy(mainArg->mutex);
						freeMem(mainArg->tknList);
						free(mainArg->tknList);
						free(mainArg);
						return 1;
					}
					ptrC->word = (char*) malloc(strlen(ptrF->word) + 1);
					ptrC->word = strcpy(ptrC->word, ptrF->word);
					ptrC->count = ptrF->count / 2;
					ptrA->count += ptrF->count * log10(ptrF->count / ptrC->count);
					ptrF = ptrF->nextTkn;
					ptrC->nextTkn = NULL;
					ptrE->nextTkn = ptrC;
					ptrE = ptrE->nextTkn;
				}
				// append any remaining tokens in second file's token list to end of current mean list
				while (ptrG) {
					ptrC = (token_t*) malloc(sizeof(token_t));
					if (!ptrC) {	
						puts("Out of memory");
						freeMem(meanList);
						pthread_mutex_destroy(mainArg->mutex);
						freeMem(mainArg->tknList);
						free(mainArg->tknList);
						free(mainArg);
						return 1;
					}
					ptrC->word = (char*) malloc(strlen(ptrG->word) + 1);
					ptrC->word = strcpy(ptrC->word, ptrG->word);
					ptrC->count = ptrG->count / 2;
					ptrB->count += ptrG->count * log10(ptrG->count / ptrC->count);
					ptrG = ptrG->nextTkn;
					ptrC->nextTkn = NULL;
					ptrE->nextTkn = ptrC;
					ptrE = ptrE->nextTkn;
				}
				// set current column's head token struct to contain the JSD
				ptrD->count = (ptrA->count + ptrB->count) / 2;
				// create first struct for next column for next pair of files to compare
				ptrE = (token_t*) malloc(sizeof(token_t));
				// set next column in mean-structure to point to above sruct
				ptrD->nextFile = ptrE;
				// reset token counts for files A and B to their original values
				ptrA->count = countForA;
				ptrB->count = countForB;
				// go to next successive file to compare with file A
				ptrB = ptrB->nextFile;
			}
			// go to next file for next round of unique comparisons
			ptrA = ptrA->nextFile;
		}
		// last struct's nextFile as NULL and free extra allocated struct
		ptrD->nextFile = NULL;
		free(ptrE);
		// sort mean-structure list, print results, and free memory used by the mean-structure
		sortList(meanList);
		printValues(meanList);
		freeMem(meanList);
	}

	// destroy mutex and free all remaining memory
	pthread_mutex_destroy(mainArg->mutex);
	freeMem(mainArg->tknList);
	free(mainArg->tknList);
	free(mainArg);

	return 0;
}
