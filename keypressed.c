/*** includes ***/
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include <errno.h>
#include <sys/ioctl.h>

/*** defines ***/
#define CTRL_KEY(k) ((k) & 0x1f)
#define KILO_VERSION "0.0.1"

enum editorKey{
	ARROW_LEFT = 1000,
	ARROW_RIGHT,
	ARROW_DOWN,
	ARROW_UP,
	PAGE_UP,
	PAGE_DOWN
};
/*** data ***/

struct editorConfig{
	int cx, cy;
	int screenrows;
	int screencols;
	struct termios orig_termios;
};

struct editorConfig E;

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
		printf("%d", c);
		printf("%d : ('%c')", c, c);
	}
	/* if (c == '\x1b'){ */
	/* 	char seq[3]; */
	/* 	if (read(STDOUT_FILENO, &seq[0], 1) != 1) return '\x1b'; */
	/* 	if (read(STDOUT_FILENO, &seq[1], 1) != 1) return '\x1b'; */
	/* 	if (seq[0] == '['){ */
	/* 		if (seq[1] >= '0' && seq[1] <= '9'){ */
	/* 			if (read(STDIN_FILENO, &seq[2], 1) != 1) return '\x1b'; */
	/* 			if (seq[2] == '~'){ */
	/* 				switch(seq[1]){ */
	/* 					case '5': return PAGE_UP; */
	/* 					case '6': return PAGE_DOWN; */
	/* 				} */
	/* 			} */
	/* 		} else { */
	/* 			switch (seq[1]){ */
	/* 				case 'A': return ARROW_UP; */
	/* 				case 'B': return ARROW_DOWN; */
	/* 				case 'C': return ARROW_RIGHT; */
	/* 				case 'D': return ARROW_LEFT; */
	/* 			} */
	/* 		} */
	/* 	} */
	/* 	return '\x1b'; */
	/* } else { */
	/* 	return c; */
	/* } */
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
	switch(key){
		case ARROW_LEFT:
			if (E.cx != 0) E.cx--;
			break;
		case ARROW_UP:
			if (E.cy != 0) E.cy--;
			break;
		case ARROW_DOWN:
			if (E.cy != E.screenrows - 1) E.cy++;
			break;
		case ARROW_RIGHT:
			if (E.cx != E.screencols - 1) E.cx++;
			break;
	}
}

void editorProcessKeypress(){
	int c = editorReadKey();
	switch(c){
		case CTRL_KEY('q'):
			write(STDOUT_FILENO, "\x1b[2J", 4);
			write(STDOUT_FILENO, "\x1b[H", 3);
			exit(0);
			break;
		// PAGE UP DOWN
		case PAGE_UP:
		case PAGE_DOWN:
			{
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
	}
}

/*** output ***/


void editorDrawRows(struct abuf *ab){
	int y;
	for (y = 0; y < E.screenrows; y++){
		if (y == E.screenrows / 3){
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
		} else {
			abAppend(ab, "~", 1);
		}
		abAppend(ab, "\x1b[K", 3);
		if (y < E.screenrows - 1) abAppend(ab, "\r\n", 2);

	}
}

void editorRefreshScreen(){
	struct abuf ab = ABUF_INIT;
	// Disable cursor (prevent blinking middle page)
	abAppend(&ab, "\x1b[?25l", 6);
	abAppend(&ab, "\x1b[H", 3);
	editorDrawRows(&ab);
	// Cursor Position	
	char buf[32];
	snprintf(buf, sizeof(buf), "\x1b[%d;%dH", E.cy + 1, E.cx + 1);
	abAppend(&ab, buf, strlen(buf));

	abAppend(&ab, "\x1b[?25h", 6);
	//Enable Cursor 
	write(STDOUT_FILENO, ab.b, ab.len);
	abFree(&ab);
}
/*** init ***/

void initEditor(){
	E.cx = 0;
	E.cy = 0;
	if (getWindowSize(&E.screenrows, &E.screencols) == -1) bust("getWindowSize");
}

int main(){
	enableRawMode();
	/* initEditor(); */
	while (1){
		char c = '\0';
		if (read(STDIN_FILENO, &c, 1) == -1 && errno != EAGAIN) bust("read");
		if (iscntrl(c)){
			printf("%d\r\n", c);
		}
		else {
			printf("%d ('%c')\r\n", c, c);
		}
		if (c == CTRL_KEY('q')) break;

		/* editorRefreshScreen(); */
		/* editorProcessKeypress(); */

	}
	return 0;
}
