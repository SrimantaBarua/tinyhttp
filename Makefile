tinyhttp: tinyhttp.c
	gcc -o $@ $< -Wall -Wextra -O2

clean:
	rm tinyhttp

