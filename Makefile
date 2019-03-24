CC?=gcc

tinyhttp: tinyhttp.c
	$(CC) -o $@ $< -Wall -Wextra -O2 -lpthread

clean:
	rm -f tinyhttp

