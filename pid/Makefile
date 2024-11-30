RAYLIB=`pkg-config --cflags --libs raylib`

pid: pid.c
	cc pid.c -o pid -fsanitize=undefined $(RAYLIB) -lm
