@echo off
cd v3kn
for /f %%f in ('dir *.cpp *.h /b/s') do "C:\Program Files\Microsoft Visual Studio\18\Insiders\VC\Tools\Llvm\x64\bin\clang-format" -i %%f
cd ..
