# Simple Experiment

1. Compile
```bash
g++ -O0 -o simple simple.cpp
```
2. Run PIN tracer
```bash
$PIN_ROOT/pin -t /nethome/kshan9/pin_dir/ChampSim/tracer/pin/obj-intel64/champsim_trac.so   -s 130000000 -t 20000000 -o champsim.trace -- ./simple
```

3. Run sst
```bash
mpirun -n 2 sst pool.py 2>&1 | tee tmpout.txt
```