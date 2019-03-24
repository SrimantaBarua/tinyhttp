tinyhttp: tinyhttp.c
	gcc -o $@ $< -Wall -Wextra -O2 -lpthread

clean:
	rm tinyhttp

