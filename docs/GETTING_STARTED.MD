# Step 1

Compile Liballocs in the contrib directory the usual way, make sure you set the `--enable-lifetime-policies` flag.

TODO: Makefile in contrib directory

# Step 2

Compile PyCAllocs with `make`. 

# Step 3 

Ensure you have liballocs metadata for your libc and Python. 

KNOWN ISSUE: Due to `libdwarfpp` limitations, you may need to compile Python with GCC 10 or earlier, otherwise this will not work.

# Step 4

Run the tests in the test directory with `./run_tests.sh <test_name>` or all of them with `make`.

TODO: ˙make˙is currently broken