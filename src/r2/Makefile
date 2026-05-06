all:
	$(MAKE) plugin

plugin user-install user-uninstall:
	$(MAKE) -C ../.. $@

clean:
	rm -f *.dylib *.so *.dSYM

.PHONY: all clean
