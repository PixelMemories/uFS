# NOTES
Please ensure there is no corrupted DISKFILE present before mounting. The program is a
little finicky and may fail its getattr or readdir leading to a mounting failure. If it does,
please end the mounting process, run a make clean and rm DISKFILE, then remount and
the benchmarks should run.

# TO USE
In /uFS/
1. make clean
2. rm DISKFILE
3. make
4. ./rufs -s /tmp/<NETID>/mountdir -d

We then open a new terminal in /uFS/
1. cd benchmarks/
2. make clean
3. make
4. run either ./simple_test or ./test_case
