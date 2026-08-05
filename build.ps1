$ErrorActionPreference = "Stop"
Write-Host "Compiling ForgeLSM Library..."
$srcFiles = Get-ChildItem src/*.cpp -Exclude "*.git_head"
$objs = @()
foreach ($file in $srcFiles) {
    Write-Host "Compiling $($file.Name)"
    $objFile = "build/$($file.BaseName).o"
    g++ -std=c++20 -Iinclude -mcx16 -O3 -c $file.FullName -o $objFile
    $objs += $objFile
}
Write-Host "Creating static library libForgeLSM.a"
ar rcs build/libForgeLSM.a $objs

Write-Host "Compiling Benchmarks..."
$benchmarks = @("bench_engine", "advanced_benchmarks", "putbatch_bench", "chaos_target", "wisckey_waf_bench", "bloom_filter_bench", "colloquium_benchmarks", "readwhilewriting_bench", "cold_read_bench", "sustained_overwrite_bench", "vlog_gc_impact_bench", "group_commit_qd_bench", "crash_recovery_bench", "ycsb_full_matrix_bench", "tombstone_penalty_bench")
foreach ($bench in $benchmarks) {
    Write-Host "Compiling $bench"
    g++ -std=c++20 -Iinclude -mcx16 -O3 "benchmarks/${bench}.cpp" -Lbuild -lForgeLSM -pthread -latomic -o "build/${bench}.exe"
}

Write-Host "Compiling Demos..."
$demos = @("demo_crud", "demo_crypto", "demo_gc", "demo_iot", "demo_iot2", "demo_gc2")
foreach ($demo in $demos) {
    Write-Host "Compiling $demo"
    g++ -std=c++20 -Iinclude -mcx16 -O3 "demos/${demo}.cpp" -Lbuild -lForgeLSM -pthread -latomic -o "build/${demo}.exe"
}
Write-Host "Build Complete!"
