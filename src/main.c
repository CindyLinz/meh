
#define _GNU_SOURCE
#include <unistd.h>

#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>

#include "meh.h"

#define MODE_NORM 0
#define MODE_LIST 1
#define MODE_CTL 2
static int mode;

extern Display *display;

/* Supported Formats */
extern struct imageformat libjpeg;
extern struct imageformat giflib;
extern struct imageformat libpng;
extern struct imageformat bmp;
struct imageformat *formats[] = {
	&libjpeg,
	&bmp,
	&libpng,
	&giflib, /* HACK! make gif last (uses read()) */
	NULL
};


void usage(){
	printf("USAGE: meh [FILE1 [FILE2 [...]]]\n");
	printf("       meh -list                 : treat stdin as list of files\n");
	printf("       meh -ctl                  : display files as they are received on stdin\n");
	exit(EXIT_FAILURE);
}

struct image *newimage(FILE *f){
	struct image *img = NULL;
	struct imageformat **fmt = formats;
	for(fmt = formats; *fmt; fmt++){
		if((img = (*fmt)->open(f))){
			img->fmt = *fmt;
			img->ximg = NULL;
			img->redraw = 1;
			img->state = NONE;
			return img;
		}
	}
	return NULL;
}

struct image *imgopen(FILE *f){
	return newimage(f);
}

/* For MODE_CTL */
int ctlfd;

static int imageslen;
static int imageidx;
static char **images;

//static char *filename = NULL;
static int width = 0, height = 0;
static struct image *img = NULL;

void freeimage(struct image *img){
	if(img){
		if(img->ximg)
			freeXImg(img->ximg);
		if(img->buf)
			free(img->buf);
		free(img);
	}
}

void nextimage(){
	if(++imageidx == imageslen)
		imageidx = 0;
}

void previmage(){
	if(--imageidx < 0)
		imageidx = imageslen - 1;
}

static void (*direction)() = nextimage;

void handlekeypress(XEvent *event){
	KeySym key = XLookupKeysym(&event->xkey, 0);
	switch(key){
		case XK_Escape:
		case XK_q:
			exit(0);
			break;
		case XK_Return:
			puts(images[imageidx]);
			fflush(stdout);
			break;
		case XK_j:
		case XK_k:
			if(mode == MODE_CTL)
				return;
			direction = key == XK_j ? nextimage : previmage;
			direction();
			/* Pass through */
			freeimage(img);
			img = NULL;
			break;
		case XK_r:
			freeimage(img);
			img = NULL;
			break;
	}
}

void handleevent(XEvent *event){
	switch(event->type){
		case ConfigureNotify:
			if(width != event->xconfigure.width || height != event->xconfigure.height){
				width = event->xconfigure.width;
				height = event->xconfigure.height;
				if(img){
					if(img->ximg)
						XDestroyImage(img->ximg);
					img->ximg = NULL;
					img->redraw = 1;
					if(img->state == LINEARDRAWN)
						img->state = LINEAR;
					else if(img->state == BILINEARDRAWN)
						img->state = BILINEAR;
				}

				/* Some window managers need reminding */
				if(img)
					setaspect(img->bufwidth, img->bufheight);
			}
			break;
		case Expose:
			if(img){
				img->redraw = 1;
				if(img->state == LINEARDRAWN)
					img->state = LINEAR;
				else if(img->state == BILINEARDRAWN)
					img->state = BILINEAR;
			}
			break;
		case KeyPress:
			if(mode == MODE_CTL)
				break;
			handlekeypress(event);
			break;
	}
}

struct image *imageopen2(){
	char *filename = images[imageidx];
	struct image *i;
	FILE *f;
	if((f = fopen(filename, "rb"))){
		if((i = imgopen(f)))
			return i;
		else
			fprintf(stderr, "Invalid format '%s'\n", filename);
	}else{
		fprintf(stderr, "Cannot open '%s'\n", filename);
	}
	return NULL;
}

int doredraw(struct image **i){
	if(!*i){
		if(mode == MODE_CTL){
			if(images[0] == NULL)
				return 0;
			if(!(*i = imageopen2(images[0]))){
				images[0] = NULL;
				return 0;
			}
		}else{
			int firstimg = imageidx;
			while(!*i){
				if((*i = imageopen2(images[imageidx]))){
					break;
				}
				if(mode == MODE_CTL)
					return 0;
				direction();
				if(imageidx == firstimg){
					fprintf(stderr, "No valid images to view\n");
					exit(EXIT_FAILURE);
				}
			}
		}

		setaspect((*i)->bufwidth, (*i)->bufheight);

		(*i)->buf = malloc(3 * (*i)->bufwidth * (*i)->bufheight);
		if((*i)->fmt->read(*i)){
			fprintf(stderr, "read error!\n");
		}
		(*i)->fmt->close(*i);
		return 1;
	}else if(width && height){
		if(!(*i)->ximg){
			(*i)->ximg = getimage(*i, width, height);
			return 1;
		}else if((*i)->redraw){ /* TODO */
			drawimage((*i)->ximg, width, height);
			(*i)->redraw = 0;
			return 1;
		}
	}
	return 0;
}

void run(){
	int xfd;
	xfd = ConnectionNumber(display);
	fd_set fds;
	struct timeval tv0 = {0, 0};
	struct timeval *tv = &tv0;
	int maxfd = xfd > ctlfd ? xfd : ctlfd;

	for(;;){
		FD_ZERO(&fds);
		FD_SET(xfd, &fds);
		if(mode == MODE_CTL)
			FD_SET(ctlfd, &fds); /* STDIN */
		int ret = select(maxfd+1, &fds, NULL, NULL, tv);
		if(ret == -1){
			perror("select failed\n");
		}
		if(FD_ISSET(ctlfd, &fds)){
			assert(mode == MODE_CTL);
			size_t slen = 0;
			ssize_t read;
			if(images[0]){
				free(images[0]);
			}
			freeimage(img);
			img = NULL;
			images[0] = NULL;
			if((read = getline(images, &slen, stdin)) == -1){
				exit(EXIT_SUCCESS);
			}
			images[0][read-1] = '\0';
		}
		if(XPending(display)){
			tv = &tv0;
			XEvent event;
			do{
				XNextEvent(display, &event);
				handleevent(&event);
			}while(XPending(display));
		}else if(ret == 0){
			if(mode == MODE_CTL && images[0] == NULL){
					tv = NULL;
			}else if(!img || img->state != BILINEARDRAWN){
				if(!doredraw(&img)){
					tv = NULL;
				}
			}
		}
	}
}

void readlist(FILE *f){
	int lsize = 1;
	imageslen = 0;
	images = NULL;
	while(!feof(f)){
		images = realloc(images, lsize * sizeof(char *));
		while(imageslen < lsize && !feof(f)){
			char *line = NULL;
			size_t slen = 0;
			ssize_t read;
			if((read = getline(&line, &slen, f)) > 0){
				line[read-1] = '\0';
				images[imageslen++] = line;
			}
		}
		lsize *= 2;
	}
}

int main(int argc, char *argv[]){
	if(argc < 2)
		usage();

	if(!strcmp(argv[1], "-ctl")){
		if(argc != 2)
			usage();
		mode = MODE_CTL;
		images = malloc(sizeof(char *));
		images[0] = NULL;
		imageslen = 1;
		imageidx = 0;
		ctlfd = 0;
	}else if(!strcmp(argv[1], "-list")){
		mode = MODE_LIST;
		if(argc == 2){
			readlist(stdin);
		}else if(argc == 3){
			FILE *f = fopen(argv[2], "r");
			readlist(f);
			fclose(f);
		}else{
			usage();
		}
	}else{
		mode = MODE_NORM;
		images = &argv[1];
		imageslen = argc-1;
		imageidx = 0;
	}
	xinit();
	run();

	return 0;
}

