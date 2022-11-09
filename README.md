Introduction
============

__minify__ is an executable compressor.  You give it an executable and it
gives you back the same executable, but smaller.

If you care about having a small executable, first design the program around
making it small, then compress it.  Here are some approaches to create small
executables, which will compress even better:

* Use compiler and linker flags to prefer small executables, e.g. options like
  "optimize for size", link-time optimization, disable stack frame pointers, etc.
* (C++) Disable exceptions and run-time type information (RTTI).
* (C++) Avoid standard library and templates that will blow up the code.
* Avoid copying objects unnecessarily.
* (Windows) Disable using standard library (MSVCRT) and use a minimal replacement.
* Precompile as much data as possible, either using compiler-generated code
  or external tools/scripts.
* Use static/global data when feasible instead of filling out structures in code.


Status
======

The current goal is to make it work for Windows executables on x86_32 and x86_64
architectures.


How it works
============

Here are the steps taken by the compressor to create a small executable:

1. Load the binary/executable into memory in the same way as the dynamic
   linker would load it.
2. Fill unneeded data directories with zeros, for example debug directory,
   exception table, etc.  This process is partially destructive, but the goal
   is to create as small executable as possible.
3. Convert import address table (IAT) to a simpler format, so that we can
   compress it and then fill it out manually during decompression.  Normally
   the dynamic linker/loader would take care of this, but we want to compress
   it.
4. Append code for loading DLLs and filling out the original import address table.
5. Disassemble text/code segment/section and convert it into individual streams
   for instructions, operands, jump targets, etc.  Use absolute jump targets
   instead of relative.
6. Append code for restoring the text/code segment/section.
7. Use a common compression algorithm to pack the data from the above points.
   Find repeated sequences of bytes and encode them as (distance-length) pairs.
   This is called [LZ77 encoding](https://en.wikipedia.org/wiki/LZ77_and_LZ78).
   Subsequent packet types, literals, distances and lenghts are stored in
   separate streams.  The stream of literals only has lower 7 bits of each byte.
   There is a separate stream which encodes differences in the 8th bit of each
   subsequent literal.
8. Append code for decompressing the above.
9. Apply arithmetic coding to encode entropy of the data from steps 7 and 8.
10. Append code for arithmetic decoding.
11. Write out the above to a PE file, but in a way that minimizes the file size,
    for example, fold the PE header into the MZ header, generate a single section,
    don't allow relocation (use fixed image base), etc.

When the produced executable is loaded by the system, it simply follows the
above steps in reverse to get back the original executable, then jumps to the
orignal entry point.
