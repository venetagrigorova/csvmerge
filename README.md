# Efficient Programs 2024w Task Implementation
## Group 14

Members:

## Usage

### On your personal maching

-Clone repository to your machine
-Compile file using 
```
gcc <file.c> -o <file>
```
- Run using 
```
./<file> data/f1.csv data/f2.csv data/f3.csv data/f4.csv`
./<file> data/a.csv data/b.csv data/c.csv data/d.csv`
```
- Test using 
```
./<file> data/f1.csv data/f2.csv data/f3.csv data/f4.csv |sort|diff - output.csv
./<file> data/a.csv data/b.csv data/c.csv data/d.csv` |sort|diff - abcd.csv
```
### On g0 machine

- Connect to g0 machine via ssh
- Copy your file to your personal home folder via scp or similar
- Compile file
```
gcc <file.c> -o <file>
```
- Run using 
```
./<file> /localtmp/efficient24/f1.csv /localtmp/efficient24/f2.csv /localtmp/efficient24/f3.csv /localtmp/efficient24/f4.csv`
./<file> /localtmp/efficient24/a.csv /localtmp/efficient24/b.csv /localtmp/efficient24/c.csv /localtmp/efficient24/d.csv`
```
- Test using 
```
./<file> /localtmp/efficient24/f1.csv /localtmp/efficient24/f2.csv /localtmp/efficient24/f3.csv /localtmp/efficient24/f4.csv |sort|diff - output.csv
./<file> /localtmp/efficient24/a.csv /localtmp/efficient24/b.csv /localtmp/efficient24/c.csv /localtmp/efficient24/d.csv` |sort|diff - abcd.csv
```
- Performance measuring using
```
perf stat ./<file> /localtmp/efficient24/f1.csv /localtmp/efficient24/f2.csv /localtmp/efficient24/f3.csv /localtmp/efficient24/f4.csv >/dev/null
```
  
# Documentation
Put performance of _joinV1.c_ tested on g0 into log file _joinV1.txt_ in same directory
