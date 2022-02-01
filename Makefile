freqtop: freqtop.c
	$(CC) -D_POSIX_C_SOURCE=199309L -std=c99 -Wall -g -o freqtop freqtop.c -lm

clean:
	rm -f ./freqtop

