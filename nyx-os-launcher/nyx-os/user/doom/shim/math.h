// Shim math.h — soft implementations live in nyx_doom.c.
#ifndef NYX_SHIM_MATH_H
#define NYX_SHIM_MATH_H

double atan2(double y, double x);
double atan(double x);
double sqrt(double x);
double pow(double b, double e);
double floor(double x);
double ceil(double x);
double fabs(double x);
double sin(double x);
double cos(double x);
double tan(double x);
double log(double x);
double log10(double x);
double exp(double x);
double fmod(double a, double b);
double round(double x);

#endif
