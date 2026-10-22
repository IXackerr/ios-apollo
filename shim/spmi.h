#pragma once
/*
 * SPMI PMIC arbiter v5 reads through the observer channel (sweet and
 * apollo share the arbiter layout). Returns the byte, or -1.
 */
int spmi_rd(unsigned sid, unsigned addr);
/* APID of a peripheral (PPID = SID << 8 | peripheral), or 0xffff. */
unsigned spmi_apid(unsigned ppid);
