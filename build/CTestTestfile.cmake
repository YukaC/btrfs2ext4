# CMake generated Testfile for 
# Source directory: /home/yuka/Proyectos/btrfs2ext4
# Build directory: /home/yuka/Proyectos/btrfs2ext4/build
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
add_test(stress_test "/home/yuka/Proyectos/btrfs2ext4/build/test_stress")
set_tests_properties(stress_test PROPERTIES  _BACKTRACE_TRIPLES "/home/yuka/Proyectos/btrfs2ext4/CMakeLists.txt;133;add_test;/home/yuka/Proyectos/btrfs2ext4/CMakeLists.txt;0;")
add_test(fuzz_test "/home/yuka/Proyectos/btrfs2ext4/build/test_fuzz")
set_tests_properties(fuzz_test PROPERTIES  _BACKTRACE_TRIPLES "/home/yuka/Proyectos/btrfs2ext4/CMakeLists.txt;134;add_test;/home/yuka/Proyectos/btrfs2ext4/CMakeLists.txt;0;")
