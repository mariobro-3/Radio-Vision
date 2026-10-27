idf_component_register(
    SRCS "rv_dsp.c"
    INCLUDE_DIRS "include")

# The DSP core is plain C99 + libm; keep it optimized and warnings-clean.
target_compile_options(${COMPONENT_LIB} PRIVATE -O2 -Wall -Wextra)
