Introduction
============

__minify__ is an executable compressor.  You give it an executable and it
gives you back the same executable, but smaller.

If you care about having a small executable, first design the program around
making it small, then compress it.  Here are some tips to create small
executables, which will compress even better:

* Use compiler and linker flags to prefer small executables, e.g. options like
  "optimize for size", link-time optimization, disable stack frame pointers, etc.
* (C++) Disable exceptions and run-time type information (RTTI).
* (C++) Avoid standard library and use templates only in ways which don't blow up
  generated code.
* Avoid copying objects unnecessarily.
* (Windows) Disable using standard library (MSVCRT) and use a minimal replacement.
* Precompile as much data as possible, either using compiler-generated code
  or external tools/scripts.
* Use static/global data when feasible instead of filling out structures in code.


Status
======

__minify__ currently works for Windows executables on x86_32 and x86_64
architectures.

Code rearrangement hasn't been implemented yet.


Limitations
===========

__minify__ works only for executables and not for DLLs.

The compression is partially destructive:
* Relocations are removed, so the executable can only be loaded at a fixed
  address in memory.
* Exception table is removed, so Structural Exception Handling won't work.


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
9. Apply [arithmetic coding](https://en.wikipedia.org/wiki/Arithmetic_coding)
   to encode entropy of the data from steps 7 and 8.
10. Append code for arithmetic decoding.
11. Write out the above to a PE file, but in a way that minimizes the file size,
    for example, fold the PE header into the MZ header, generate a single section,
    don't allow relocation (use fixed image base), etc.

When the produced executable is loaded by the system, it simply follows the
above steps in reverse order to get back the original executable, then jumps to the
orignal entry point.


How compression works
=====================

Compression removes repetitions in the data.  Input data which is compressible
has low entropy (low noise), so it looks like many repeating patterns of data.
Compressed data has high entropy (high noise), so it looks more random.

The first step to compress data is to convert it to a format which has high rate
of repetitions.  As an example, image and video compressors reorganize 2D data
into 1-dimensional, then apply FFT to convert it to frequency/amplitude domain, where
it is easier to find repeating patterns (e.g. usually low frequencies remain and
high frequencies which look like noise either don't exist or can be filtered-out).
Anyway, this step depends on the data to be compressed.  Each type of data can
be transformed in a different way to achieve a format which has lower entropy and
is thus easier to compress.

The second step is to find repeating sequences of bytes.  This produces a sequence
of repeating and non-repeating data.  Next this information has to be encoded
in a space-efficient way.  Finding the best way to encode repetitions is quite
challenging.

The last step is to encode entropy.  This exploits statistical properties of the
output from the previous step.  Symbols (typically bytes or bits) are encoded
by using less bits for symbols with higher probability of occurrence vs. more bits
for symbols occurring rarely.  This can be achieved using Huffman coding,
arithmetic coding or range coding.

The biggest difficulty lies in putting these steps together.  For example, the way
the data is encoded in the second step may achieve better or worse compression
in the third step, depending on which repeating squences are selected.  As an example,
in LZMA compression, the range coder in the third step drives the finding and selection
of repetitions in the second step to achieve better compression.
