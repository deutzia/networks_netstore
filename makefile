COMPILER = g++
CCFLAGS = -Wall -Wextra -std=c++17 -O0 -g
LFLAGS = -lboost_program_options -lboost_filesystem -lboost_system
STUDENT = ik394380

SRCS = netstore-client.cc netstore-server.cc helper.cc
OBJS = $(SRCS:.cc=.o)

FILES = $(wildcard *.cc) $(wildcard *.h) makefile readme.txt

DEPDIR := .d
DEPFLAGS = -MT $@ -MMD -MP -MF $(DEPDIR)/$*.d
$(shell mkdir -p $(DEPDIR) >/dev/null)

COMPILE.cc = $(COMPILER) $(DEPFLAGS) $(CCFLAGS) -c

all : netstore-client netstore-server

%.o : %.cc $(DEPDIR)/%.d
		$(COMPILE.cc) -c $< -o $@

netstore-client : netstore-client.o helper.o
		$(COMPILER) $(CCFLAGS) $(LFLAGS) netstore-client.o helper.o -o netstore-client

netstore-server : netstore-server.o helper.o
		$(COMPILER) $(CCFLAGS) $(LFLAGS) netstore-server.o helper.o -o netstore-server

${STUDENT}.tar.gz: ${SOURCES}
		mkdir ${STUDENT}
		cp ${FILES} ${STUDENT}
		tar cvzf ${STUDENT}.tar.gz ${STUDENT}

clean:
		@rm -f $(OBJS) netstore-client netstore-server
		@rm -rf .d/

$(DEPDIR)/%.d: ;
.PRECIOUS: $(DEPDIR)/%.d

include $(wildcard $(patsubst %,$(DEPDIR)/%.d,$(basename $(SRCS))))

