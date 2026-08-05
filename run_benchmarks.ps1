$ErrorActionPreference = "Continue"

Remove-Item -Force benchmark_results.txt -ErrorAction SilentlyContinue

$benchmarks = @("bench_engine", "advanced_benchmarks", "putbatch_bench", "chaos_target", "wisckey_waf_bench", "bloom_filter_bench", "colloquium_benchmarks", "readwhilewriting_bench")
foreach ($bench in $benchmarks) {
    Write-Host "Running $bench..."
    echo "`n--- $bench ---`n" >> benchmark_results.txt
    $exe = ".\build\${bench}.exe"
    Invoke-Expression "$exe >> benchmark_results.txt 2>&1"
}

$demos = @("demo_crud", "demo_crypto", "demo_gc", "demo_iot")
foreach ($demo in $demos) {
    Write-Host "Running $demo..."
    echo "`n--- $demo ---`n" >> benchmark_results.txt
    $exe = ".\build\${demo}.exe"
    Invoke-Expression "$exe >> benchmark_results.txt 2>&1"
}

Write-Host "All benchmarks and demos finished."
