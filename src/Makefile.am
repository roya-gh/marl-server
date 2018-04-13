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

AUTOMAKE_OPTIONS = subdir-objects

ACLOCAL_AMFLAGS = ${ACLOCAL_FLAGS} -I m4

AM_CXXFLAGS = --pedantic -Wall -Werror

bin_PROGRAMS = marl-server

marl_server_CPPFLAGS = \
    $(MARL_PROTOCOLS_CFLAGS) \
    $(FLOG_CFLAGS) \
    $(CPNET_CFLAGS)

marl_server_LDADD = \
    -lpthread \
    $(MARL_PROTOCOLS_LIBS) \
    $(FLOG_LIBS) \
    $(CPNET_LIBS)

marl_server_SOURCES = \
    server.cpp \
    server.hpp \
    definitions.hpp \
    concurrentqueue.h \
    blockingconcurrentqueue.h \
    main.cpp
