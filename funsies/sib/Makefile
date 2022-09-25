.PHONY: all clean

BFM=/proc/sys/fs/binfmt_misc
REG=${BFM}/register
BFS=${BFM}/sib
ULB=/usr/local/bin
SIB=${ULB}/sib

all: ${BFS} ${SIB}

${SIB}: sib.c | ${ULB}
	${CC} -o $@ $^

${ULB}:
	mkdir -p $@

${BFM}:
	mkdir -p $@

${REG}: ${BFM}
	mount binfmt_misc -t binfmt_misc ${BFM}

${BFS}: ${REG}
	cat binfmt_sib > ${REG}

clean:
	rm -f ${SIB}
