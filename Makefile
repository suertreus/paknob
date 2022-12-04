DEPS="libpulse absl_any_invocable absl_str_format absl_strings absl_span"

all: paknob

format: paknob.cc
	clang-format -i --style=Google $^

iwyu:
	include-what-you-use -Xiwyu --no_comments -Xiwyu --no_fwd_decls -std=c++17 paknob.cc `pkgconf --cflags $(DEPS)`

paknob: paknob.o
	$(CXX) $(CXXFLAGS) -std=c++17 -o $@ $^ `pkgconf --libs ${DEPS}`

paknob.o: paknob.cc
	$(CXX) $(CXXFLAGS) -std=c++17 -c -o $@ $< `pkgconf --cflags ${DEPS}`

clean:
	rm -f paknob *.o

install: paknob
	install -D $< --target-directory="$(DESTDIR)/usr/bin"

.PHONY: clean all format iwyu install
