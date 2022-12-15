DEPS="libpulse absl_any_invocable absl_str_format absl_strings absl_span"

all: paknob

format: paknob.cc
	clang-format -i --style=Google $^

iwyu:
	include-what-you-use -Xiwyu --no_comments -Xiwyu --no_fwd_decls -std=c++17 paknob.cc `pkg-config --cflags $(DEPS)`

paknob: paknob.o
	$(CXX) $(CXXFLAGS) -std=c++17 -o $@ $^ `pkg-config --libs ${DEPS}`

paknob.o: paknob.cc
	$(CXX) $(CXXFLAGS) -std=c++17 -c -o $@ $< `pkg-config --cflags ${DEPS}`

clean:
	rm -f paknob *.o

install: paknob
	install -D $< --target-directory="$(DESTDIR)/usr/bin"

homedir-install: paknob
	install -D $< --target-directory="$(HOME)/bin"

.PHONY: clean all format iwyu install homedir-install
