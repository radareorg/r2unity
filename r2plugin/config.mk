EXTERNAL_PLUGINS+=r2unity
# EXTERNAL_PLUGINS+=hi

.PHONY: r2unity

r2unity: p/r2unity

p/r2unity:
	cd p && git clone https://github.com/trufae/r2unity
