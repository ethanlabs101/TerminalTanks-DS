/* Correctly rounded sin/cos/log/hypot/cube for builds whose libm is not (the DS). See xmath.c. */
#ifndef TT_XMATH_H
#define TT_XMATH_H
double x_sin(double x);
double x_cos(double x);
double x_log(double x);
double x_hypot(double x, double y);
double x_cube(double t);

/* core/ calls these names. On the host (glibc, the libm Python itself uses in the parity tests) they are the
 * libm functions; with TT_XMATH (the DS build) they are the double-double versions. */
#ifdef TT_XMATH
#define py_sin x_sin
#define py_cos x_cos
#define py_log x_log
#define py_hypot x_hypot
#define py_cube x_cube
#else
#define py_sin sin
#define py_cos cos
#define py_log log
#define py_hypot hypot
#define py_cube(t) pow((t), 3)
#endif
#endif
