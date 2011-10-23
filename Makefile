CC = gcc
OBJECTS = twofingemu.o gestures.o easing.o
LIBS = -lm -lpthread -lXtst -lXrandr -lX11 -lXi
CFLAGS = -Wall -O2
BINDIR = $(DESTDIR)/usr/bin
NAME = twofing

twofing: $(OBJECTS)
	$(CC) -o $(NAME) $(OBJECTS) $(LIBS)

%.o: %.c
	$(CC) -c $(CFLAGS) $<

install:
	install --mode=755 $(NAME) $(BINDIR)/
	cp 70-touchscreen-egalax.rules $(DESTDIR)/etc/udev/rules.d/

clean:
	rm *.o $(NAME)

uninstall:
	rm $(BINDIR)/$(NAME)
