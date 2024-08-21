/*** includes ***/
#define _GNU_SOURCE
#define _BSD_SOURCE
/* #define _DEFAULT_SOURCE */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <time.h>
#include <stdarg.h>

/*** defines ***/
#define CTRL_KEY(k) ((k) & 0x1f)
#define KILO_VERSION "0.0.1"
#define TAB_STOP 8
#define QUIT_TIMES 3 

enum editorKey{
	BACKSPACE = 127,
	ARROW_LEFT = 1000,
	ARROW_RIGHT,
	ARROW_DOWN,
	ARROW_UP,
	PAGE_UP,
	PAGE_DOWN,
	HOME_KEY,
	END_KEY,
	DEL_KEY,
};
/*** data ***/

typedef struct erow{
	int size;
	int rsize;
	char *strings;
	char *render;
} erow;

struct editorConfig{
	int cx, cy;
	int rx; //Render field horizontal position
	//scroll
	int rowoff;
	int coloff;
	//window size
	int screenrows;
	int screencols;
	//number of rows
	int nrows;
	int dirty;
	char *filename;
	//Status message 
	char statusmsg[80];
	time_t statusmsg_time;
	//buffer contains text lines
	erow *row;
	struct termios orig_termios;
};

struct editorConfig E;

/*** prototypes ***/

void editorUpdateRow(erow *row);
void editorAppendRow(char *s, size_t len);
void editorSetStatusMessage(const char *fmt, ...);
void editorSave();

/*** terminal ***/
void bust(const char *s){
	write(STDOUT_FILENO, "\x1b[2J", 4);
	write(STDOUT_FILENO, "\x1b[H", 3);
	
	perror(s);
	exit(1);
}


void disableRawMode(){
	if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios) == -1) bust("tcsetattr");
}
void enableRawMode(){
	if (tcgetattr(STDIN_FILENO, &E.orig_termios) == -1) bust("tcgetattr");
	atexit(disableRawMode);
	struct termios raw = E.orig_termios;
	//Disable start/stop output control
	raw.c_iflag &= ~(IXON | ICRNL | BRKINT | INPCK | ISTRIP);
	//Disable echo, canonical mode & signals & extended input 
	raw.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN);
	//Disable output processing
	raw.c_oflag &= ~(OPOST);
	raw.c_cflag |= (CS8);
	raw.c_cc[VMIN] = 0;
	raw.c_cc[VTIME] = 1;
	if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) bust("tcgetattr");
}

int editorReadKey(){
	int nread;
	char c;
	while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
		if (nread == -1 && errno != EAGAIN) bust("read");
	}
	if (c == '\x1b'){
		char seq[3];
		if (read(STDOUT_FILENO, &seq[0], 1) != 1) return '\x1b';
		if (read(STDOUT_FILENO, &seq[1], 1) != 1) return '\x1b';
		if (seq[0] == '['){
			if (seq[1] >= '0' && seq[1] <= '9'){
				if (read(STDIN_FILENO, &seq[2], 1) != 1) return '\x1b';
				if (seq[2] == '~'){
					switch(seq[1]){
						// \x1b[5~
						case '1': return HOME_KEY;
						case '3': return DEL_KEY;
						case '4': return END_KEY;
						case '5': return PAGE_UP;
						case '6': return PAGE_DOWN;
						case '7': return HOME_KEY;
						case '8': return END_KEY;
					}
				}
			} else {
				switch (seq[1]){
					// \x1b[A
					case 'A': return ARROW_UP;
					case 'B': return ARROW_DOWN;
					case 'C': return ARROW_RIGHT;
					case 'D': return ARROW_LEFT;
					case 'H': return HOME_KEY;
					case 'F': return END_KEY;
				}
			}
		} else if (seq[0] == 'O'){
			switch (seq[1]){
				// \x1bOH
				case 'H': return HOME_KEY;
				case 'F': return END_KEY;
			}
		}
		return '\x1b';
	} else {
		return c;
	}
}

int getCursorPosition(int *rows, int *cols){
	char buf[32];
	unsigned int i = 0;
	if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4) return -1;

	while (i < sizeof(buf) - 1){
		if (read(STDIN_FILENO, &buf[i], 1) != 1) break;
		if (buf[i] == 'R') break;
		i++;
	}
	buf[i] = '\0';
	if (buf[0] != '\x1b' || buf[1] != '[') return -1;
	if (sscanf(&buf[2], "%d;%d", rows, cols) != 2) return -1;

	return 0;
}

int getWindowSize(int *rows, int *cols){
	struct winsize ws;
	if ((ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1) || (ws.ws_col == 0)){
		if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) return -1;
		return getCursorPosition(rows, cols);
	} else {
		*cols = ws.ws_col;
		*rows = ws.ws_row;
		return 0;
	}
}

/*** row operations ***/

int editorRowCxToRx(erow *row, int cx){
	int rx = 0;
	int j;
	for (j = 0; j < cx; j++){
		if (row->strings[j] == '\t') rx += (TAB_STOP - 1) - (rx % TAB_STOP);
		rx++;
	}
	return rx;
}
void editorRowInsertChar(erow *row, int at, int c){
	if (at < 0 || at > row->size) at = row->size;
	row->strings = realloc(row->strings, row->size + 2);
	memmove(&row->strings[at + 1], &row->strings[at], row->size - at + 1);
	row->size++;
	row->strings[at] = c;
	editorUpdateRow(row);
	E.dirty++;
}

void editorRowDelChar(erow *row, int at){
	if (at < 0 || at >= row->size) return;
	memmove(&row->strings[at], &row->strings[at + 1], row->size - at);
	row->size--;
	editorUpdateRow(row);
	E.dirty++;
}

void editorFreeRow(erow *row){
	free(row->render);
	free(row->strings);
}

void editorDelRow(int at){
	if (at < 0 || at >= E.nrows) return;
	editorFreeRow(&E.row[at]);
	memmove(&E.row[at], &E.row[at+1], sizeof(erow) * (E.nrows - at - 1));
	E.nrows--;
	E.dirty++;
}

void editorRowAppendString(erow *row, char *s, size_t len){
	row->strings = realloc(row->strings, row->size + len + 1);
	memcpy(&row->strings[row->size], s, len);
	row->size += len;
	row->strings[row->size] = '\0';
	editorUpdateRow(row);
	E.dirty++;
}
void editorInsertRow(int at, char *s, size_t len){
	if (at < 0 || at >= E.nrows) return;
	E.row = realloc(E.row, sizeof(erow) * (E.nrows + 1));
	memmove(&E.row[at+1], &E.row[at], sizeof(erow) * (E.nrows - at));
	E.row[at].size = len;
	E.row[at].strings = malloc(len+1);
	memcpy(E.row[at].strings, s, len);
	E.row[at].strings[len] = '\0';
	E.row[at].rsize = 0;
	E.row[at].render = NULL;
	editorUpdateRow(&E.row[at]);
	E.nrows++;
	E.dirty++;
}

/*** editor operations ***/

void editorInsertChar(int c){
	if (E.cy == E.nrows) editorInsertRow(E.nrows, "", 0);
	editorRowInsertChar(&E.row[E.cy], E.cx, c);
	E.cx++;
}
void editorInsertNewLine(){
	if (E.cx == 0){
		editorInsertRow(E.cy, "", 0);
	} else {
		erow *row = &E.row[E.cy];
		editorInsertRow(E.cy+1, &row->strings[E.cx], row->size - E.cx);
		row = &E.row[E.cy];
		row->size = E.cx;
		row->strings[row->size] = '\0';
		editorUpdateRow(row);
	}
	E.cy++;
	E.cx = 0;
}

void editorDelChar(){
	if (E.cy == E.nrows) return;
	if (E.cx == 0 && E.cy == 0) return;

	erow *row = &E.row[E.cy];

	if (E.cx > 0){
		editorRowDelChar(row, E.cx-1);
		E.cx--;
	} else {
		E.cx = E.row[E.cy - 1].size;
		editorRowAppendString(&E.row[E.cy - 1], row->strings, row->size);
		editorDelRow(E.cy);
		E.cy--;
	}
}

/*** append buffer ***/

struct abuf{
	char *b;
	int len;
};

#define ABUF_INIT {NULL, 0}

void abAppend(struct abuf* ab, const char *s, int len){
	char *new = realloc(ab->b, ab->len + len);
	if (new == NULL) return;
	memcpy(&new[ab->len], s, len);
	ab->b = new;
	ab->len += len;	
}

void abFree(struct abuf *ab){
	free(ab->b);
}

/*** input ***/

void editorMoveCursor(int key){
	erow *row = (E.cy >= E.nrows) ? NULL : &E.row[E.cy];
	switch(key){
		case ARROW_LEFT:
			if (E.cx != 0) E.cx--;
			else if  (E.cy > 0){
				E.cy--;
				E.cx = E.row[E.cy].size;
			}
			break;
		case ARROW_UP:
			if (E.cy != 0) E.cy--;
			break;
		case ARROW_DOWN:
			if (E.cy < E.nrows) E.cy++;
			break;
		case ARROW_RIGHT:
			if (row && (E.cx < row->size)) E.cx++;
			else if (row && E.cx == row->size){
				E.cy++;
				E.cx = 0;
			}
			break;
	}
	row = (E.cy >= E.nrows) ? NULL : &E.row[E.cy];
	int rowlen = row ? row->size : 0;
	if (E.cx > rowlen){
		E.cx = rowlen;
	}
	
}

void editorProcessKeypress(){
	static int quit_times = QUIT_TIMES;
	int c = editorReadKey();
	switch(c){
		case '\r': 
			editorInsertNewLine();
			break;
		case CTRL_KEY('q'):
			if (E.dirty && quit_times > 0){
				editorSetStatusMessage("WARNING!!! File has unsaved changes. "
						"Press Ctrl-Q %d more times to quit.", quit_times);
				quit_times--;
				return;
			}
			exit(0);
			break;
		//HOME END KEY
		case HOME_KEY:
			E.cx = 0;
			break;
		case END_KEY:
			if (E.cy < E.nrows) E.cx = E.row[E.cy].size;
			break;

		case BACKSPACE:
		case CTRL_KEY('h'):
		case DEL_KEY:
			if (c == DEL_KEY) editorMoveCursor(ARROW_RIGHT);
			editorDelChar();
			break;
		// PAGE UP DOWN
		case PAGE_UP:
		case PAGE_DOWN:
			{
				if (c == PAGE_UP){
					E.cy = E.rowoff;
				} else if (c == PAGE_DOWN) {
					E.cy = E.rowoff + E.screenrows - 1;
					if (E.cy > E.nrows) E.cy = E.nrows;
				}
				int times = E.screenrows;
				while (times--)
					editorMoveCursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
			}
			break;
		// Cursor position
		case ARROW_UP:
		case ARROW_LEFT:
		case ARROW_DOWN:
		case ARROW_RIGHT:
			editorMoveCursor(c);
			break;
		case CTRL_KEY('l'):
		case '\x1b':
			break;
		
		case CTRL_KEY('s'):
			editorSave();
			break;	
		default:
			editorInsertChar(c);
			break;
	}
	quit_times = QUIT_TIMES;
}

/*** output ***/

void editorScroll(){
	// Cursor goes past upper limit, back off by 1 line
	E.rx = 0;
	if (E.cy < E.nrows){
		E.rx = editorRowCxToRx(&E.row[E.cy], E.cx);
	}
	if (E.cy < E.rowoff){
		E.rowoff = E.cy;
	}
	// Cursor goes past bottom limit, scroww more by 1 line
	if (E.cy >= E.rowoff + E.screenrows){
		E.rowoff = E.cy - E.screenrows + 1;
	}
	if (E.rx < E.coloff){
		E.coloff = E.rx;
	}
	if (E.rx >= E.coloff + E.screencols){
		E.coloff = E.rx - E.screencols+ 1;
	}
}

void editorDrawRows(struct abuf *ab){
	int y;
	for (y = 0; y < E.screenrows; y++){
		int filerow = y + E.rowoff;
		if (filerow  >= E.nrows){
		if (E.nrows == 0 && y == E.screenrows / 3){
			char welcome[80];
			int welcomelen = snprintf(welcome, sizeof(welcome), "Kilo editor -- version %s", KILO_VERSION);
			if (welcomelen > E.screencols) welcomelen = E.screencols;
			int padding = (E.screencols - welcomelen) / 2;
			if (padding){
				abAppend(ab, "~", 1);
				padding--;
			}
			while (padding--) abAppend(ab, " ", 1);
			abAppend(ab, welcome, welcomelen);
		} 		else {
			abAppend(ab, "~", 1);
		}
		} else {
			int len = E.row[filerow].rsize - E.coloff;
			if (len < 0) len = 0;
			if (len > E.screencols) len = E.screencols;
			abAppend(ab, &E.row[filerow].render[E.coloff], len);
		}
		abAppend(ab, "\x1b[K", 3);
		/* if (y < E.screenrows - 1) abAppend(ab, "\r\n", 2); */
		abAppend(ab, "\r\n", 2); 	
	}
}

void editorDrawStatusBar(struct abuf *ab){
	abAppend(ab, "\x1b[7m", 4);
	char status[80], rstatus[80];
	int len = snprintf(status, sizeof(status), "%.20s - %d lines %s", E.filename ? E.filename : "[No Name]", E.nrows, E.dirty ? "(modified)" : "");	
	int rlen = snprintf(rstatus, sizeof(rstatus), "%d/%d", E.cy + 1, E.nrows);
	if (len > E.screencols) len = E.screencols;
	abAppend(ab, status, len);
	while (len < E.screencols){
		if (E.screencols - len == rlen){
			abAppend(ab, rstatus, rlen);
			break;
		} else {
			abAppend(ab, " ", 1);
			len++;
		}
	}
	abAppend(ab, "\x1b[m", 3);
	abAppend(ab, "\r\n", 2);
}

void editorDrawMessageBar(struct abuf *ab){
	abAppend(ab, "\x1b[K", 3);
	int msglen = strlen(E.statusmsg);
	if (msglen > E.screencols) msglen = E.screencols;
	if (msglen && (time(NULL) - E.statusmsg_time < 5)) abAppend(ab, E.statusmsg, msglen);
}

void editorRefreshScreen(){
	editorScroll();
	struct abuf ab = ABUF_INIT;
	// Disable cursor (prevent blinking middle page)
	abAppend(&ab, "\x1b[?25l", 6);
	abAppend(&ab, "\x1b[H", 3);
	editorDrawRows(&ab);
	editorDrawStatusBar(&ab);
	editorDrawMessageBar(&ab);
	// Cursor Position	
	char buf[32];
	snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (E.cy - E.rowoff) + 1, (E.rx - E.coloff) + 1);
	abAppend(&ab, buf, strlen(buf));

	abAppend(&ab, "\x1b[?25h", 6);
	//Enable Cursor 
	write(STDOUT_FILENO, ab.b, ab.len);
	abFree(&ab);
}
void editorSetStatusMessage(const char *fmt, ...){
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(E.statusmsg, sizeof(E.statusmsg), fmt, ap);
	va_end(ap);
	E.statusmsg_time = time(NULL);
}	

/*** file i/o ***/

void editorUpdateRow(erow *row){
	int tabs = 0;
	int j;
	for (j = 0; j < row->size; j++)
		if (row->strings[j] == '\t') tabs++;
	free(row->render);
	row->render = malloc(row->size + tabs * (TAB_STOP - 1) + 1);

	int idx = 0;
	for (j = 0; j < row->size; j++){
		if (row->strings[j] == '\t'){
			row->render[idx++] = ' ';
			while (idx % TAB_STOP != 0) row->render[idx++] = ' ';
		} else {
			row->render[idx++] = row->strings[j];
		}
	}
	row->render[idx] = '\0';
	row->rsize = idx;
}

char *editorRowsToString(int *buflen){
	int ttlen = 0;
	int j;
	for (j = 0; j < E.nrows; j++) 
		ttlen += E.row[j].size + 1;
	*buflen = ttlen;
	char *buf = malloc(ttlen);
	char *p = buf;
	for (j = 0; j < E.nrows; j++){
		memcpy(p, E.row[j].strings, E.row[j].size);
		p += E.row[j].size;
		*p = '\n';
		p++;
	}
	return buf;
}

void editorSave(){
	if (E.filename == NULL) return;
	int len;
	char *buf = editorRowsToString(&len);
	int fd = open(E.filename, O_RDWR | O_CREAT, 0644);
	if (fd != -1){
		if (ftruncate(fd, len) != -1){
			if (write(fd, buf, len) == len){
				close(fd);
				free(buf);
				E.dirty = 0;
				editorSetStatusMessage("%d bytes written to disk", len);
				return;
			}
		}
		close(fd);
	}
	free(buf);
	editorSetStatusMessage("Can't write to file I/O error: %s", strerror(errno));
}

//Append new line into row
void editorAppendRow(char *s, size_t len){
	E.row = realloc(E.row, sizeof(erow) * (E.nrows + 1));
	int idx = E.nrows;
	E.row[idx].size = len;
	E.row[idx].strings = malloc(len + 1);
	memcpy(E.row[idx].strings, s, len);
	E.row[idx].strings[len] = '\0';
	E.row[idx].rsize = 0;
	E.row[idx].render = NULL;
	editorUpdateRow(&E.row[idx]);
	E.nrows++;
	E.dirty++;
}

void editorOpen(char *filename){
	/* char *line = "Hello, world!"; */
	/* ssize_t len = 13; */
	/* E.row.size = len; */
	/* E.row.strings = malloc(len + 1); */
	/* memcpy(E.row.strings, line, len); */
	/* E.row.strings[len] = '\0'; */
	/* E.nrows = 1; */
	free(E.filename);
	E.filename = strdup(filename);
	FILE *fp = fopen(filename, "r");
	if (!fp) bust("fopen");
	char *line = NULL;
	size_t linecap = 0;
	ssize_t linelen;
	while((linelen = getline(&line, &linecap, fp)) != -1) {
		while (linelen > 0 && (line[linelen - 1] == '\n' || line[linelen - 1] == '\r')) linelen--;
		editorInsertRow(E.nrows, line, linelen);
	}
	free(line);
	fclose(fp);
	E.dirty = 0;
}

/*** init ***/

void initEditor(){
	E.cx = 0;
	E.rx = 0;
	E.cy = 0;
	E.rowoff = 0;
	E.coloff = 0;
	E.row = NULL;
	E.nrows = 0;
	E.dirty = 0;
	E.filename = NULL;
	E.statusmsg[0] = '\0';
	E.statusmsg_time = 0;
	if (getWindowSize(&E.screenrows, &E.screencols) == -1) bust("getWindowSize");
	E.screenrows -= 2;
}

int main(int argc, char *argv[]){
	enableRawMode();
	initEditor();
	if (argc >= 2){
		editorOpen(argv[1]);
	}
	editorSetStatusMessage("HELP: Ctrl-S = save | Ctrl-Q = quit");
	while (1){
		/* char c = '\0'; */
		/* if (read(STDIN_FILENO, &c, 1) == -1 && errno != EAGAIN) bust("read"); */
		/* if (iscntrl(c)){ */
		/* 	printf("%d\r\n", c); */
		/* } */
		/* else { */
		/* 	printf("%d ('%c')\r\n", c, c); */
		/* } */
		/* if (c == CTRL_KEY('q')) break; */

		editorRefreshScreen();
		editorProcessKeypress();
	}
	return 0;
}
