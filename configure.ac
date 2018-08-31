# Copyright 2017 - Roya Ghasemzade <roya@ametisco.ir>
# Copyright 2017 - Soroush Rabiei <soroush@ametisco.ir>
#
# This file is part of marl_server.
#
# marl_server is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# marl_server is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with marl_server.  If not, see <http://www.gnu.org/licenses/>.

AC_PREREQ([2.69])
AC_INIT([marl_server], [0.1], [https://github.com/roya-gh/marl_server/issues])
AC_CONFIG_SRCDIR([src])
AC_CONFIG_HEADERS([config.h])

# Checks for programs.
AC_PROG_CXX
AC_PROG_CC

AC_CONFIG_MACRO_DIRS([m4])

# Configure automake
AM_INIT_AUTOMAKE
AM_SILENT_RULES([yes])

# Check for C++11 compiler
AX_CXX_COMPILE_STDCXX_11([noext],[mandatory])

# Checks for libraries.

# Checks for header files.

# Checks for typedefs, structures, and compiler characteristics.

# Checks for library functions.

PKG_CHECK_MODULES([MARL_PROTOCOLS], [libmarl_protocols >= 0.1])
PKG_CHECK_MODULES([CPNET], [libcpnet >= 1.0])
PKG_CHECK_MODULES([FLOG], [libflog >= 0.1],[])

AC_CONFIG_FILES([Makefile
                 src/Makefile])

AC_OUTPUT
