/*
 * AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA
 * - Lumin, 2022
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

#define MAX_SZ (128)

int main(int argc, char* argv[])
{
	int i;
	int fd;
	char* lf;
	char* nxt;
	char buff[MAX_SZ + 1] = { 0x00 };
	char* arr[(MAX_SZ >> 1) + 2] = { 0x00 };

	if (
		argc < 2
		|| (fd = open(argv[1], O_RDONLY | O_CLOEXEC)) < 0
		|| lseek(fd, 4, SEEK_SET) == (off_t)-1
		|| read(fd, buff, MAX_SZ) < 2
	)
	{
		return -1;
	}
	lf = strchr(buff, '\n');
	if (!lf)
	{
		lf = buff + MAX_SZ;
	}
	*lf = 0x00;
	nxt = buff;
	for (i = 0; nxt && nxt < lf; i++)
	{
		arr[i] = nxt;
		nxt = strchr(nxt, ' ');
		if (nxt)
		{
			*nxt = 0x00;
			nxt++;
		}
	}
	arr[i] = argv[1];
	arr[++i] = NULL;
	execv(arr[0], arr);
	close(fd);
	return -1;
}
