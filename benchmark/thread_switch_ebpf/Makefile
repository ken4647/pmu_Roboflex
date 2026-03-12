all:
	gcc -o ./build/taskA taskA.c -O0
	sudo setcap cap_sys_nice+ep ./build/taskA
	gcc -o ./build/taskB taskB.c -O0
	sudo setcap cap_sys_nice+ep ./build/taskB

clean:
	rm -f ./build/taskA
	rm -f ./build/taskB

.PHONY: all clean