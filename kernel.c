#include "stm32f10x.h"
#include "RTOSConfig.h"

#include "syscall.h"

#include <stddef.h>

void *memcpy(void *dest, const void *src, size_t n);

int strcmp(const char *a, const char *b) __attribute__ ((naked));
int strcmp(const char *a, const char *b)
{
	asm(
        "strcmp_lop:                \n"
        "   ldrb    r2, [r0],#1     \n"
        "   ldrb    r3, [r1],#1     \n"
        "   cmp     r2, #1          \n"
        "   it      hi              \n"
        "   cmphi   r2, r3          \n"
        "   beq     strcmp_lop      \n"
		"	sub     r0, r2, r3  	\n"
        "   bx      lr              \n"
		:::
	);
}


//this code is reference from the c library
int strncmp(const char *a, const char *b, size_t n)
{
    if ( n == 0 )
        return 0;

    do {
        if( *a != *b++ )
            return *a - *--b;
        if( *a++ == 0 )
            break;
    } while( --n != 0 );

    return 0;
}


size_t strlen(const char *s) __attribute__ ((naked));
size_t strlen(const char *s)
{
	asm(
		"	sub  r3, r0, #1			\n"
        "strlen_loop:               \n"
		"	ldrb r2, [r3, #1]!		\n"
		"	cmp  r2, #0				\n"
        "   bne  strlen_loop        \n"
		"	sub  r0, r3, r0			\n"
		"	bx   lr					\n"
		:::
	);
}

void puts(char *s)
{
	while (*s) {
		while (USART_GetFlagStatus(USART2, USART_FLAG_TXE) == RESET)
			/* wait */ ;
		USART_SendData(USART2, *s);
		s++;
	}
}

#define STACK_SIZE 512 /* Size of task stacks in words */
#define TASK_LIMIT 8  /* Max number of tasks we can handle */
#define PIPE_BUF   64 /* Size of largest atomic pipe message */
#define PATH_MAX   32 /* Longest absolute path */
#define PIPE_LIMIT (TASK_LIMIT * 2)

#define PATHSERVER_FD (TASK_LIMIT + 3) 
	/* File descriptor of pipe to pathserver */

#define PRIORITY_DEFAULT 20
#define PRIORITY_LIMIT (PRIORITY_DEFAULT * 2 - 1)

#define TASK_READY      0
#define TASK_WAIT_READ  1
#define TASK_WAIT_WRITE 2
#define TASK_WAIT_INTR  3
#define TASK_WAIT_TIME  4

#define S_IFIFO 1
#define S_IMSGQ 2

#define O_CREAT 4


/* Stack struct of user thread, see "Exception entry and return" */
struct user_thread_stack {
	unsigned int r4;
	unsigned int r5;
	unsigned int r6;
	unsigned int r7;
	unsigned int r8;
	unsigned int r9;
	unsigned int r10;
	unsigned int fp;
	unsigned int _lr;	/* Back to system calls or return exception */
	unsigned int _r7;	/* Backup from isr */
	unsigned int r0;
	unsigned int r1;
	unsigned int r2;
	unsigned int r3;
	unsigned int ip;
	unsigned int lr;	/* Back to user thread code */
	unsigned int pc;
	unsigned int xpsr;
	unsigned int stack[STACK_SIZE - 18];
};

/* Task Control Block */
struct task_control_block {
    struct user_thread_stack *stack;
    int pid;
    int status;
    int priority;
    struct task_control_block **prev;
    struct task_control_block  *next;
};

// I can't find the solution to avoid using global variables!!!
struct task_control_block tasks[TASK_LIMIT];
size_t task_count = 0;



/* 
 * pathserver assumes that all files are FIFOs that were registered
 * with mkfifo.  It also assumes a global tables of FDs shared by all
 * processes.  It would have to get much smarter to be generally useful.
 *
 * The first TASK_LIMIT FDs are reserved for use by their respective tasks.
 * 0-2 are reserved FDs and are skipped.
 * The server registers itself at /sys/pathserver
*/
#define PATH_SERVER_NAME "/sys/pathserver"
void pathserver()
{
	char paths[PIPE_LIMIT - TASK_LIMIT - 3][PATH_MAX];
	int npaths = 0;
	int i = 0;
	unsigned int plen = 0;
	unsigned int replyfd = 0;
	char path[PATH_MAX];

	memcpy(paths[npaths++], PATH_SERVER_NAME, sizeof(PATH_SERVER_NAME));

	while (1) {
		read(PATHSERVER_FD, &replyfd, 4);
		read(PATHSERVER_FD, &plen, 4);
		read(PATHSERVER_FD, path, plen);

		if (!replyfd) { /* mkfifo */
			int dev;
			read(PATHSERVER_FD, &dev, 4);
			memcpy(paths[npaths], path, plen);
			mknod(npaths + 3 + TASK_LIMIT, 0, dev);
			npaths++;
		}
		else { /* open */
			/* Search for path */
			for (i = 0; i < npaths; i++) {
				if (*paths[i] && strcmp(path, paths[i]) == 0) {
					i += 3; /* 0-2 are reserved */
					i += TASK_LIMIT; /* FDs reserved for tasks */
					write(replyfd, &i, 4);
					i = 0;
					break;
				}
			}

			if (i >= npaths) {
				i = -1; /* Error: not found */
				write(replyfd, &i, 4);
			}
		}
	}
}

int mkfile(const char *pathname, int mode, int dev)
{
	size_t plen = strlen(pathname)+1;
	char buf[4+4+PATH_MAX+4];
	(void) mode;

	*((unsigned int *)buf) = 0;
	*((unsigned int *)(buf + 4)) = plen;
	memcpy(buf + 4 + 4, pathname, plen);
	*((int *)(buf + 4 + 4 + plen)) = dev;
	write(PATHSERVER_FD, buf, 4 + 4 + plen + 4);

	return 0;
}

int mkfifo(const char *pathname, int mode)
{
	mkfile(pathname, mode, S_IFIFO);
	return 0;
}

int open(const char *pathname, int flags)
{
	unsigned int replyfd = getpid() + 3;
	size_t plen = strlen(pathname) + 1;
	unsigned int fd = -1;
	char buf[4 + 4 + PATH_MAX];
	(void) flags;

	*((unsigned int *)buf) = replyfd;
	*((unsigned int *)(buf + 4)) = plen;
	memcpy(buf + 4 + 4, pathname, plen);
	write(PATHSERVER_FD, buf, 4 + 4 + plen);
	read(replyfd, &fd, 4);

	return fd;
}

int mq_open(const char *name, int oflag)
{
	if (oflag & O_CREAT)
		mkfile(name, 0, S_IMSGQ);
	return open(name, 0);
}

void serialout(USART_TypeDef* uart, unsigned int intr)
{
	int fd;
	char c;
	int doread = 1;
	mkfifo("/dev/tty0/out", 0);
	fd = open("/dev/tty0/out", 0);

	while (1) {
		if (doread)
			read(fd, &c, 1);
		doread = 0;
		if (USART_GetFlagStatus(uart, USART_FLAG_TXE) == SET) {
			USART_SendData(uart, c);
			USART_ITConfig(USART2, USART_IT_TXE, ENABLE);
			doread = 1;
		}
		interrupt_wait(intr);
		USART_ITConfig(USART2, USART_IT_TXE, DISABLE);
	}
}

void serialin(USART_TypeDef* uart, unsigned int intr)
{
	int fd;
	char c;
	mkfifo("/dev/tty0/in", 0);
	fd = open("/dev/tty0/in", 0);

    USART_ITConfig(USART2, USART_IT_RXNE, ENABLE);

	while (1) {
		interrupt_wait(intr);
		if (USART_GetFlagStatus(uart, USART_FLAG_RXNE) == SET) {
			c = USART_ReceiveData(uart);
			write(fd, &c, 1);
		}
	}
}



#define COLOR_R "\033[0;31m"
#define COLOR_G "\033[0;32m"
#define COLOR_B "\033[0;34m"
#define DEFAULT_C "\033[0m"


void greeting()
{
    write_str( "\n\r" );
    write_str( COLOR_R );
	write_str( "Hello, World!\n" );
    write_str( DEFAULT_C ); // recover original color
}


void help()
{
    write_str( "\n\r" );
    write_str( COLOR_G );

    write_str( "List of commands:\n" );
    write_str( "\r\thello\t- show a welcom message with colors.\n" );
    write_str( "\r\techo\t- show a message you entered.\n" );
    write_str( "\r\tps\t- show all running task.\n" );
    write_str( "\r\thelp\t- show the list of commands.\n" );
    write_str( "\r\thistory- show the command histoy.\n" );

    write_str( DEFAULT_C ); // recover original color
}



void echo( char *str )
{
    str += 5;       // escape "echo "
    write_str( "\n\r" );
    write_str( COLOR_R );

    int type;  // 0->Nothing, 1->Error on '\'', 2-> Error on '\"', 3-> Esacap front and tail
    int end = strlen(str)-1;

    if( (str[0] == '\'' || str[end] == '\'') &&  str[0] != str[end] ) 
        type = 1;
    else if( (str[0] == '\"' || str[end] == '\"') &&  str[0] != str[end] ) 
        type = 2;
    else if( str[0] == '\'' || str[0] == '\"' ) 
        type = 3;
    else
        type = 0;

    if( type == 1 )
        write_str( "Unmatched \'." );
    else if( type == 2 ) 
        write_str( "Unmatched \"." );
    else if( type == 3 ) {
        char buf[100] = {0};
        str++;  // escape front '\'' or '\"'
        memcpy( buf, str, strlen(str)-1 );
        write_str( buf );
    }
    else 
        write_str( str );

    write_str( DEFAULT_C );
}

void rs232_xmit_msg_task()
{
	int fdout, fdin;
	char str[100];
	int curr_char;
	fdout = open("/dev/tty0/out", 0);
	fdin = mq_open("/tmp/mqueue/out", O_CREAT);
	setpriority(0, PRIORITY_DEFAULT - 2);

	while (1) {
		/* Read from the queue.  Keep trying until a message is
		 * received.  This will block for a period of time (specified
		 * by portMAX_DELAY). */
		read(fdin, str, 100);

		/* Write each character of the message to the RS232 port. */
		curr_char = 0;
		while (str[curr_char] != '\0') {
			write(fdout, &str[curr_char], 1);
			curr_char++;
		}
	}
}

void queue_str_task(const char *str, int delay)
{
	int fdout = mq_open("/tmp/mqueue/out", 0);
	int msg_len = strlen(str) + 1;

	while (1) {
		/* Post the message.  Keep on trying until it is successful. */
		write(fdout, str, msg_len);

		/* Wait. */
		sleep(delay);
	}
}

void write_ch( char ch )
{
    int fdout = mq_open("/tmp/mqueue/out", 0);

    char str_ch[2] = {'\0','\0'};       // initial 
    str_ch[0] = ch;                     // copy ch to str_ch

    write(fdout, str_ch, 2);
}

void write_str( char *str )
{
    int fdout = mq_open("/tmp/mqueue/out", 0);
    write( fdout, str, strlen(str)+1 );
}

char* itoa( int value ) 
{
    char str[32] = {0};

    int now = 30;
    do {
        str[now--] = "0123456789"[value%10];
        value /= 10;
    } while( value && now );

    return &str[now+1];
}

char * task_status( int status )
{
    switch( status ) {
        case TASK_READY: 
                return "\tTASK_READY"; 

        case TASK_WAIT_READ:
                return "\tTASK_WAIT_READ";

        case TASK_WAIT_WRITE:
                return "\tTASK_WAIT_WRITE";

        case TASK_WAIT_INTR:
                return "\tTASK_WAIT_INTR";

        case TASK_WAIT_TIME:
                return "\tTASK_WAIT_TIME";

        default:
            return "Error Status";
    };
}


void ps()
{
    int i;

    write_str( COLOR_G );
    write_str( "\n\rThere are total " );
    write_str( itoa( task_count ) );
    write_str( " task(s) in running!!\n" );
    write_str( DEFAULT_C );

    write_str( "\rPID\tStatus\tPriority\n\r" );

    for( i = 0; i < task_count; i++ ) {
        write_str( itoa(tasks[i].pid) );
        write_str( task_status(tasks[i].status) );
        write_str( "\t" );
        write_str( itoa(tasks[i].priority) );
        write_str( "\n\r" );
    }

}


/// Display or add command line histoy, ctl->0 for add, ctl->1 for display
#define MAX_HISTORY 5
void History( char* cmd, int ctl )
{
    static char history[MAX_HISTORY][100] = {0};
    static int front = 0;     // point to front of queue
    static int tail = 0;    // point to tail of command histoy

    if( ctl == 0 ) {    // Add to command histoy
        memcpy( history[front], cmd, strlen(cmd) );
        front = (front+1) % MAX_HISTORY;
        if( tail == front )
            tail = (tail+1) % MAX_HISTORY;
    }                   // Display all command history
    else if( ctl == 1 ) {
        write_str( COLOR_G );
        write_str( "\n\rCommand History(from new to old):" );

        int now = front - 1;
        if( now < 0 )   // special case for front = 0
            now = MAX_HISTORY - 1;
        for( ; now != tail; now-- ) {
            write_str( "\n\r\t" );
            write_str( history[now] );

            if( now == 0 )
                now = MAX_HISTORY;
        }
        write_str( "\n\r\t" );
        write_str( history[tail] );

        write_str( DEFAULT_C );
    }
    else {
        write_str( "\n\rError control in History\n" );
    }

}


void command( char *str )
{
    str+=6;  // to escape shells$

    if( strcmp( str, "hello" ) == 0 ) {
        History( str, 0 );
        greeting();
    }
    else if( strcmp( str, "ps" ) == 0 ) {
        History( str, 0 );
        ps();
    }
    else if( strcmp( str, "help" ) == 0 ) {
        History( str, 0 );
        help(); 
    }
    /*else if( strcmp( str, "\033[A" ) == 0 )
        write_str( "\n\rUP" );
    else if( strcmp( str, "\033[B" ) == 0 )
        write_str( "\n\rDOWN" );
    else if( strcmp( str, "\033[C" ) == 0 )
        write_str( "\n\rRIGHT" );
    else if( strcmp( str, "\033[D" ) == 0 )
        write_str( "\n\rLEFT" );*/
    else if ( strcmp( str, "history" ) == 0 ) {
        History( NULL, 1 );
        History( str, 0 );
    }
    else if ( strncmp( str, "echo ", 5 ) == 0 ) {
        History( str, 0 );
        echo( str );
    }
    else {
        if( strlen(str) != 0 ) { 
            write_str( COLOR_R );
            write_str( "\n\r" );
            write_str( str );
            write_str( " - Command not found" );
            write_str( DEFAULT_C );
        }
    }
}


void serial_readwrite_task()
{
	int fdout, fdin;
	char str[100];
	char ch;
	int curr_char;
	int done;

	fdout = mq_open("/tmp/mqueue/out", 0);
	fdin = open("/dev/tty0/in", 0);

    memcpy(str, "Shell$", 6);
    //write(fdout, str, 6);
    write_str( str );

	while (1) {
		curr_char = 6;
		done = 0;
		do {
			/* Receive a byte from the RS232 port (this call will
			 * block). */
			read(fdin, &ch, 1);

			/* If the byte is an end-of-line type character, then
			 * finish the string and inidcate we are done.
			 */
			if (curr_char >= 98 || (ch == '\r') || (ch == '\n')) {
				str[curr_char] = '\0';
				done = -1;
			}
            else if ( ch == 0x7f ) {
                if( curr_char > 6 ) {
                    write_str( "\b \b" );
                    curr_char--;
                }
            }
			else {
                write_ch(ch);
                str[curr_char++] = ch;
			}
		} while (!done);

        // find valid command
        command( str );

        // Display new line of shell 
        memcpy(str, "\n\rShell$", 9);
        write_str( str );
    }
}

void first()
{
	setpriority(0, 0);

	if (!fork()) setpriority(0, 0), pathserver();
	if (!fork()) setpriority(0, 0), serialout(USART2, USART2_IRQn);
	if (!fork()) setpriority(0, 0), serialin(USART2, USART2_IRQn);
	if (!fork()) rs232_xmit_msg_task();
	if (!fork()) setpriority(0, PRIORITY_DEFAULT - 10), serial_readwrite_task();

	setpriority(0, PRIORITY_LIMIT);

	while(1);
}

struct pipe_ringbuffer {
	int start;
	int end;
	char data[PIPE_BUF];

	int (*readable) (struct pipe_ringbuffer*, struct task_control_block*);
	int (*writable) (struct pipe_ringbuffer*, struct task_control_block*);
	int (*read) (struct pipe_ringbuffer*, struct task_control_block*);
	int (*write) (struct pipe_ringbuffer*, struct task_control_block*);
};

#define RB_PUSH(rb, size, v) do { \
		(rb).data[(rb).end] = (v); \
		(rb).end++; \
		if ((rb).end >= size) (rb).end = 0; \
	} while (0)

#define RB_POP(rb, size, v) do { \
		(v) = (rb).data[(rb).start]; \
		(rb).start++; \
		if ((rb).start >= size) (rb).start = 0; \
	} while (0)

#define RB_PEEK(rb, size, v, i) do { \
		int _counter = (i); \
		int _src_index = (rb).start; \
		int _dst_index = 0; \
		while (_counter--) { \
			((char*)&(v))[_dst_index++] = (rb).data[_src_index++]; \
			if (_src_index >= size) _src_index = 0; \
		} \
	} while (0)

#define RB_LEN(rb, size) (((rb).end - (rb).start) + \
	(((rb).end < (rb).start) ? size : 0))

#define PIPE_PUSH(pipe, v) RB_PUSH((pipe), PIPE_BUF, (v))
#define PIPE_POP(pipe, v)  RB_POP((pipe), PIPE_BUF, (v))
#define PIPE_PEEK(pipe, v, i)  RB_PEEK((pipe), PIPE_BUF, (v), (i))
#define PIPE_LEN(pipe)     (RB_LEN((pipe), PIPE_BUF))

unsigned int *init_task(unsigned int *stack, void (*start)())
{
	stack += STACK_SIZE - 9; /* End of stack, minus what we're about to push */
	stack[8] = (unsigned int)start;
	return stack;
}

int
task_push (struct task_control_block **list, struct task_control_block *item)
{
	if (list && item) {
		/* Remove itself from original list */
		if (item->prev)
			*(item->prev) = item->next;
		if (item->next)
			item->next->prev = item->prev;
		/* Insert into new list */
		while (*list) list = &((*list)->next);
		*list = item;
		item->prev = list;
		item->next = NULL;
		return 0;
	}
	return -1;
}

struct task_control_block*
task_pop (struct task_control_block **list)
{
	if (list) {
		struct task_control_block *item = *list;
		if (item) {
			*list = item->next;
			if (item->next)
				item->next->prev = list;
			item->prev = NULL;
			item->next = NULL;
			return item;
		}
	}
	return NULL;
}

void _read(struct task_control_block *task, struct task_control_block *tasks, size_t task_count, struct pipe_ringbuffer *pipes);
void _write(struct task_control_block *task, struct task_control_block *tasks, size_t task_count, struct pipe_ringbuffer *pipes);

void _read(struct task_control_block *task, struct task_control_block *tasks, size_t task_count, struct pipe_ringbuffer *pipes)
{
	task->status = TASK_READY;
	/* If the fd is invalid */
	if (task->stack->r0 > PIPE_LIMIT) {
		task->stack->r0 = -1;
	}
	else {
		struct pipe_ringbuffer *pipe = &pipes[task->stack->r0];

		if (pipe->readable(pipe, task)) {
			size_t i;

			pipe->read(pipe, task);

			/* Unblock any waiting writes */
			for (i = 0; i < task_count; i++)
				if (tasks[i].status == TASK_WAIT_WRITE)
					_write(&tasks[i], tasks, task_count, pipes);
		}
	}
}

void _write(struct task_control_block *task, struct task_control_block *tasks, size_t task_count, struct pipe_ringbuffer *pipes)
{
	task->status = TASK_READY;
	/* If the fd is invalid */
	if (task->stack->r0 > PIPE_LIMIT) {
		task->stack->r0 = -1;
	}
	else {
		struct pipe_ringbuffer *pipe = &pipes[task->stack->r0];

		if (pipe->writable(pipe, task)) {
			size_t i;

			pipe->write(pipe, task);

			/* Unblock any waiting reads */
			for (i = 0; i < task_count; i++)
				if (tasks[i].status == TASK_WAIT_READ)
					_read(&tasks[i], tasks, task_count, pipes);
		}
	}
}

int
fifo_readable (struct pipe_ringbuffer *pipe,
			   struct task_control_block *task)
{
	/* Trying to read too much */
	if (task->stack->r2 > PIPE_BUF) {
		task->stack->r0 = -1;
		return 0;
	}
	if ((size_t)PIPE_LEN(*pipe) < task->stack->r2) {
		/* Trying to read more than there is: block */
		task->status = TASK_WAIT_READ;
		return 0;
	}
	return 1;
}

int
mq_readable (struct pipe_ringbuffer *pipe,
			 struct task_control_block *task)
{
	size_t msg_len;

	/* Trying to read too much */
	if ((size_t)PIPE_LEN(*pipe) < sizeof(size_t)) {
		/* Nothing to read */
		task->status = TASK_WAIT_READ;
		return 0;
	}

	PIPE_PEEK(*pipe, msg_len, 4);

	if (msg_len > task->stack->r2) {
		/* Trying to read more than buffer size */
		task->stack->r0 = -1;
		return 0;
	}
	return 1;
}

int
fifo_read (struct pipe_ringbuffer *pipe,
		   struct task_control_block *task)
{
	size_t i;
	char *buf = (char*)task->stack->r1;
	/* Copy data into buf */
	for (i = 0; i < task->stack->r2; i++) {
		PIPE_POP(*pipe, buf[i]);
	}
	return task->stack->r2;
}

int
mq_read (struct pipe_ringbuffer *pipe,
		 struct task_control_block *task)
{
	size_t msg_len;
	size_t i;
	char *buf = (char*)task->stack->r1;
	/* Get length */
	for (i = 0; i < 4; i++) {
		PIPE_POP(*pipe, *(((char*)&msg_len)+i));
	}
	/* Copy data into buf */
	for (i = 0; i < msg_len; i++) {
		PIPE_POP(*pipe, buf[i]);
	}
	return msg_len;
}

int
fifo_writable (struct pipe_ringbuffer *pipe,
			   struct task_control_block *task)
{
	/* If the write would be non-atomic */
	if (task->stack->r2 > PIPE_BUF) {
		task->stack->r0 = -1;
		return 0;
	}
	/* Preserve 1 byte to distiguish empty or full */
	if ((size_t)PIPE_BUF - PIPE_LEN(*pipe) - 1 < task->stack->r2) {
		/* Trying to write more than we have space for: block */
		task->status = TASK_WAIT_WRITE;
		return 0;
	}
	return 1;
}

int
mq_writable (struct pipe_ringbuffer *pipe,
			 struct task_control_block *task)
{
	size_t total_len = sizeof(size_t) + task->stack->r2;

	/* If the write would be non-atomic */
	if (total_len > PIPE_BUF) {
		task->stack->r0 = -1;
		return 0;
	}
	/* Preserve 1 byte to distiguish empty or full */
	if ((size_t)PIPE_BUF - PIPE_LEN(*pipe) - 1 < total_len) {
		/* Trying to write more than we have space for: block */
		task->status = TASK_WAIT_WRITE;
		return 0;
	}
	return 1;
}

int
fifo_write (struct pipe_ringbuffer *pipe,
			struct task_control_block *task)
{
	size_t i;
	const char *buf = (const char*)task->stack->r1;
	/* Copy data into pipe */
	for (i = 0; i < task->stack->r2; i++)
		PIPE_PUSH(*pipe,buf[i]);
	return task->stack->r2;
}

int
mq_write (struct pipe_ringbuffer *pipe,
		  struct task_control_block *task)
{
	size_t i;
	const char *buf = (const char*)task->stack->r1;
	/* Copy count into pipe */
	for (i = 0; i < sizeof(size_t); i++)
		PIPE_PUSH(*pipe,*(((char*)&task->stack->r2)+i));
	/* Copy data into pipe */
	for (i = 0; i < task->stack->r2; i++)
		PIPE_PUSH(*pipe,buf[i]);
	return task->stack->r2;
}

int
_mknod(struct pipe_ringbuffer *pipe, int dev)
{
	switch(dev) {
	case S_IFIFO:
		pipe->readable = fifo_readable;
		pipe->writable = fifo_writable;
		pipe->read = fifo_read;
		pipe->write = fifo_write;
		break;
	case S_IMSGQ:
		pipe->readable = mq_readable;
		pipe->writable = mq_writable;
		pipe->read = mq_read;
		pipe->write = mq_write;
		break;
	default:
		return 1;
	}
	return 0;
}

int main()
{
	unsigned int stacks[TASK_LIMIT][STACK_SIZE];
	//struct task_control_block tasks[TASK_LIMIT];
	struct pipe_ringbuffer pipes[PIPE_LIMIT];
	struct task_control_block *ready_list[PRIORITY_LIMIT + 1];  /* [0 ... 39] */
	struct task_control_block *wait_list = NULL;
	//size_t task_count = 0;
	size_t current_task = 0;
	size_t i;
	struct task_control_block *task;
	int timeup;
	unsigned int tick_count = 0;

	SysTick_Config(configCPU_CLOCK_HZ / configTICK_RATE_HZ);

	init_rs232();
	__enable_irq();

	tasks[task_count].stack = (void*)init_task(stacks[task_count], &first);
	tasks[task_count].pid = 0;
	tasks[task_count].priority = PRIORITY_DEFAULT;
	task_count++;

	/* Initialize all pipes */
	for (i = 0; i < PIPE_LIMIT; i++)
		pipes[i].start = pipes[i].end = 0;

	/* Initialize fifos */
	for (i = 0; i <= PATHSERVER_FD; i++)
		_mknod(&pipes[i], S_IFIFO);

	/* Initialize ready lists */
	for (i = 0; i <= PRIORITY_LIMIT; i++)
		ready_list[i] = NULL;

	while (1) {
		tasks[current_task].stack = activate(tasks[current_task].stack);
		tasks[current_task].status = TASK_READY;
		timeup = 0;

		switch (tasks[current_task].stack->r7) {
		case 0x1: /* fork */
			if (task_count == TASK_LIMIT) {
				/* Cannot create a new task, return error */
				tasks[current_task].stack->r0 = -1;
			}
			else {
				/* Compute how much of the stack is used */
				size_t used = stacks[current_task] + STACK_SIZE
					      - (unsigned int*)tasks[current_task].stack;
				/* New stack is END - used */
				tasks[task_count].stack = (void*)(stacks[task_count] + STACK_SIZE - used);
				/* Copy only the used part of the stack */
				memcpy(tasks[task_count].stack, tasks[current_task].stack,
				       used * sizeof(unsigned int));
				/* Set PID */
				tasks[task_count].pid = task_count;
				/* Set priority, inherited from forked task */
				tasks[task_count].priority = tasks[current_task].priority;
				/* Set return values in each process */
				tasks[current_task].stack->r0 = task_count;
				tasks[task_count].stack->r0 = 0;
				tasks[task_count].prev = NULL;
				tasks[task_count].next = NULL;
				task_push(&ready_list[tasks[task_count].priority], &tasks[task_count]);
				/* There is now one more task */
				task_count++;
			}
			break;
		case 0x2: /* getpid */
			tasks[current_task].stack->r0 = current_task;
			break;
		case 0x3: /* write */
			_write(&tasks[current_task], tasks, task_count, pipes);
			break;
		case 0x4: /* read */
			_read(&tasks[current_task], tasks, task_count, pipes);
			break;
		case 0x5: /* interrupt_wait */
			/* Enable interrupt */
			NVIC_EnableIRQ(tasks[current_task].stack->r0);
			/* Block task waiting for interrupt to happen */
			tasks[current_task].status = TASK_WAIT_INTR;
			break;
		case 0x6: /* getpriority */
			{
				int who = tasks[current_task].stack->r0;
				if (who > 0 && who < (int)task_count)
					tasks[current_task].stack->r0 = tasks[who].priority;
				else if (who == 0)
					tasks[current_task].stack->r0 = tasks[current_task].priority;
				else
					tasks[current_task].stack->r0 = -1;
			} break;
		case 0x7: /* setpriority */
			{
				int who = tasks[current_task].stack->r0;
				int value = tasks[current_task].stack->r1;
				value = (value < 0) ? 0 : ((value > PRIORITY_LIMIT) ? PRIORITY_LIMIT : value);
				if (who > 0 && who < (int)task_count)
					tasks[who].priority = value;
				else if (who == 0)
					tasks[current_task].priority = value;
				else {
					tasks[current_task].stack->r0 = -1;
					break;
				}
				tasks[current_task].stack->r0 = 0;
			} break;
		case 0x8: /* mknod */
			if (tasks[current_task].stack->r0 < PIPE_LIMIT)
				tasks[current_task].stack->r0 =
					_mknod(&pipes[tasks[current_task].stack->r0],
						   tasks[current_task].stack->r2);
			else
				tasks[current_task].stack->r0 = -1;
			break;
		case 0x9: /* sleep */
			if (tasks[current_task].stack->r0 != 0) {
				tasks[current_task].stack->r0 += tick_count;
				tasks[current_task].status = TASK_WAIT_TIME;
			}
			break;
		default: /* Catch all interrupts */
			if ((int)tasks[current_task].stack->r7 < 0) {
				unsigned int intr = -tasks[current_task].stack->r7 - 16;

				if (intr == SysTick_IRQn) {
					/* Never disable timer. We need it for pre-emption */
					timeup = 1;
					tick_count++;
				}
				else {
					/* Disable interrupt, interrupt_wait re-enables */
					NVIC_DisableIRQ(intr);
				}
				/* Unblock any waiting tasks */
				for (i = 0; i < task_count; i++)
					if ((tasks[i].status == TASK_WAIT_INTR && tasks[i].stack->r0 == intr) ||
					    (tasks[i].status == TASK_WAIT_TIME && tasks[i].stack->r0 == tick_count))
						tasks[i].status = TASK_READY;
			}
		}

		/* Put waken tasks in ready list */
		for (task = wait_list; task != NULL;) {
			struct task_control_block *next = task->next;
			if (task->status == TASK_READY)
				task_push(&ready_list[task->priority], task);
			task = next;
		}
		/* Select next TASK_READY task */
		for (i = 0; i < (size_t)tasks[current_task].priority && ready_list[i] == NULL; i++);
		if (tasks[current_task].status == TASK_READY) {
			if (!timeup && i == (size_t)tasks[current_task].priority)
				/* Current task has highest priority and remains execution time */
				continue;
			else
				task_push(&ready_list[tasks[current_task].priority], &tasks[current_task]);
		}
		else {
			task_push(&wait_list, &tasks[current_task]);
		}
		while (ready_list[i] == NULL)
			i++;
		current_task = task_pop(&ready_list[i])->pid;
	}

	return 0;
}

