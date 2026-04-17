#include "syscall.h"
#include "string.h"
#include "shell.h"

#define BUFSIZE 1024
#define MAXARGS 8

// helper function for parser
char* find_terminator(char* buf) {
	char* p = buf;
	while(*p) {
		switch(*p) {
			case ' ':
			case '\0':
			case FIN:
			case FOUT:
			case PIPE:
				return p;
			default:
				p++;
				break;
		}
	}
	return p;
}

int parse(char* buf, char** argv) {
	// FIXME
	// feel free to change this function however you see fit

	int argc = 0;
	char temp;
	char *head, *end;
	head = buf;

	for(;;) { // find each argv
		while(*head == ' ') head++;

		if (*head == '\0') {
			return argc;
		}

		argv[argc++] = head;
		end = find_terminator(head);

		// inner loop handles all file redirection
		// it may be redirected multiple times
		for(;;) {
			temp = *end;
			*end = '\0';
			switch(temp) {
				case ' ':
					// while(*(++head) == ' ') ;
					// continue;

					head = end + 1;
					while(*head == ' ') head++;
					end = head;
					continue;

				case '\0':
					return argc;

				case FOUT:
					// FIXME
					// Get filename after >
					head = end + 1;
					while (*head == ' ') {
						head++;
					}

					if (*head == '\0') {
						// No output file
						return -1;
					} else {
						// CLose current STDOUT
						_close(STDOUT);

						int status = _open(STDOUT, head);

						if (status * 1 < 0) {
							// Create the file
							status = _fscreate(head);
							if (status < 0) {
								// Could not create the file
								return -1;
							} else {
								status = _open(STDOUT, head);
								if (status < 0) {
									// Could not open the file
									return -1;
								}
							}
						}

						// Go to end of the filename

						end = find_terminator(head);
						head = end;
						continue;
					}
					
				case FIN:
					// FIXME

					// Get filename after <

					head = end + 1;

					while ((*head) == ' ') {
						head++;
					}


					if ((*head) == '\0') {
						return -1;
					} else {
						_close(STDIN);

						int s = _open(STDIN, head);

						if (s < 0) {
							return -1;
						} else {
							end = find_terminator(head);
							head = end;
							continue;
						}
					}

					continue;

				case PIPE:
					// FIXME
					return -1; // Pipe is found
					break;

				default:
					*head = temp;
					break;
			}
			break;
		}

	}
}

void fix_path_func(char * path_buffer, int buffer_size, char * command) {
	if (!strchr(command, '/')) {
		// Does not have a path
		snprintf(path_buffer, buffer_size, "c/%s", command);
	} else {
		snprintf(path_buffer, buffer_size, "%s", command);
	}
}

void exec_command(char ** argv, int argc);

void handle_pipe_commands(char * buffer) {

	// Find pipe char

	char * pos = strchr(buffer, '|'); // Found in string.h

	if (!pos) {
		return; // No position found
	} else {
		(*pos) = '\0';

		char * first_cmd = buffer;

		char * second_cmd = pos;

		second_cmd++;

		// skip the spaces in the front

		while ((*second_cmd) == ' ') {
			second_cmd++;
		}

		// Get the first command

		char * first_arg[MAXARGS + 1];
		char * second_arg[MAXARGS + 1];

		// Create the pipe

		int pipe_file_desc[2];

		int status = _pipe(&(pipe_file_desc[1]), &(pipe_file_desc[0]));

		if(status < 0) {
			// Could not make the pipe
			return;
		} else {
			int read_file_desc = pipe_file_desc[0];
			int write_file_desc = pipe_file_desc[1];

			int first_process_id = _fork();

			if (first_process_id < 0) {
				// Fork failed
				_close(read_file_desc);
				_close(write_file_desc);
				return;
			} else if (first_process_id == 0) {
				// First child writes to pipe

				_close(STDOUT);
				_uiodup(write_file_desc, STDOUT);

				_close(write_file_desc);
				_close(read_file_desc);

				// Find the exec first cmd

				int first_argc = parse(first_cmd, first_arg);

				if (first_argc > 0) {
					char path_buffer[256];

					fix_path_func(path_buffer, sizeof(path_buffer), first_arg[0]);

					int executable_file_descriptor = _open(-1, path_buffer);

					if (executable_file_descriptor < 0) {
						// Cannot execute

						_exit();
					} else {
						_exec(executable_file_descriptor, first_argc, first_arg);
					}

					_exit();
				} else {
					_exit();
				}
			}


			// Fork the second child

			int second_proc_id = _fork();

			if (second_proc_id < 0) {
				// Could not fork
				_close(read_file_desc);
				_close(write_file_desc);
				_wait(first_process_id);
				return;
			} else {
				if (!second_proc_id) {
					// read from pipe

					_close(STDIN);

					_uiodup(read_file_desc, STDIN);

					_close(read_file_desc);

					_close(write_file_desc);

					// Parse + exec second command

					int second_argc = parse(second_cmd, second_arg);

					if (second_argc <= 0) {
						_exit();
					} else {
						char path_buffer[256];

						fix_path_func(path_buffer, sizeof(path_buffer), second_arg[0]);

						int executable_file_descriptor = _open(-1, path_buffer);

						if (executable_file_descriptor >= 0) {
							_exec(executable_file_descriptor, second_argc, second_arg);
							_exit();
						} else {
							// Cannot execute

							_exit();
						}
					}
				}

				_close(read_file_desc);
				_close(write_file_desc);
				_wait(first_process_id);
				_wait(second_proc_id);
			}	
		}
	}
}

void exec_command(char ** argv, int argc) {
	if (!argc) {
		return;
	} else if(!argv[0]) {
		return;
	} else if (argv[0][0] == '\0') {
		return;
	} else {
		char path_buffer[256]; // Fix the path first

		fix_path_func(path_buffer, sizeof(path_buffer), argv[0]);

		int fork_id = _fork();

		if (fork_id < 0) {
			return;
		} else if (!fork_id) {

			// int executable_file_descriptor = _open(-1, path_buffer);

			int executable_file_descriptor = -1;

			int fd = 0;

			while (fd < 16) {
				fd = _open(fd, path_buffer);

				if (fd > -1) {
					executable_file_descriptor = fd;
					break;
				} else if (fd == -2) {
					// -EBUSY so continue
					fd++;
					continue;
				} else {
					// Some other error so just exit
					break; 
				}
			}

			if (executable_file_descriptor < 0) {
				_exit();
			} else {
				// Execute

				_exec(executable_file_descriptor, argc, argv);

				_exit();
			}
		}

		_wait(fork_id);
	}
}



int main()
{
    char buf[BUFSIZE];
	int argc;
	char* argv[MAXARGS + 1]; 

  	_open(CONSOLEOUT, "dev/uart1");		// console device
	_close(STDIN);              		// close any existing stdin
	_uiodup(CONSOLEOUT, STDIN);      	// stdin from console
	_close(STDOUT);              		// close any existing stdout
	_uiodup(CONSOLEOUT, STDOUT);     	// stdout to console

	printf("Starting 391 Shell\n");

	for (;;)
	{
		printf("LUMON OS> ");
		getsn(buf, BUFSIZE - 1);

		if (0 == strcmp(buf, "exit"))
		_exit();

		// FIXME
		// Call your parse function and exec the user input

		char * s = buf;

		while ((*s) == ' ') {
			s++;
		}

		if ((*s) == '\0') {
			continue;
		} else {
			char * end = s + (strlen(s) - 1); // -1 to avoid counting null terminator

			while (end > s && (*end) == ' ') {
				end--;
			}

			*(end + 1) = '\0';

			// Move trim to start

			if (s != buf) {
				char* source = s;
				char * d = buf;

				while ((*source)) {
					(*d) = (*source);
					d++;
					source++;
				}

				*d = '\0';
			} 

			if (0 == strcmp(buf, "exit"))
			_exit();

			if ('\0' == buf[0]) {
				continue;				
			} else {
				if (strchr(buf, '|')) {
					handle_pipe_commands(buf);
				} else {
					// No pipe

					// Just parse and exec

					int old_stdout = _open(11, "dev/uart1");

					int old_stdin = _open(10, "dev/uart1");

					
					_uiodup(STDIN, old_stdin);
					_uiodup(STDOUT, old_stdout);

					argc = parse(buf, argv);

					if (argc > 0) {
						exec_command(argv, argc);
					} 

					_close(STDIN);
					_uiodup(old_stdin, STDIN);
					_close(STDOUT);
					_uiodup(old_stdout, STDOUT);
					_close(old_stdin);
					_close(old_stdout);
				}
			}

		}
	}
}