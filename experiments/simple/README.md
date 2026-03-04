# Simple Experiment

1. Compile
```bash
g++ -O0 -o simple simple.cpp
g++ -O0 -o simple_rw simple_rw.cpp
```
2. Run PIN tracer
```bash
$PIN_ROOT/pin -t /nethome/kshan9/pin_dir/ChampSim/tracer/pin/obj-intel64/champsim_tracer.so   -s 130000000 -t 20000000 -o champsim.trace -- ./simple
$PIN_ROOT/pin -t /nethome/kshan9/pin_dir/ChampSim/tracer/pin/obj-intel64/champsim_tracer.so   -s 130000000 -t 20000000 -o champsim.trace -- ./rw --iters 10000000 --load-pct 50 --cxl-pct 25 --mem-ops 1 --compute-ops 0
```

3. Run sst
```bash
mpirun -n 2 sst pool.py 2>&1 | tee tmpout.txt
```
