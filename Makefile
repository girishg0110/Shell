OUTPUT = shell
FLAGS = -g -Wvla -Wall -fsanitize=address

all: ; gcc $(FLAGS) -o $(OUTPUT) $(OUTPUT).c

clean: ; rm -rf $(OUTPUT) *.tmp
