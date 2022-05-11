#
# This is a GNU-make Makefile for Envtool's './doc'
#
DOC_GENERATED    = envtool.chm envtool.chw doxygen.log
OUTPUT_GENERATED = $(addprefix output/, \
                     *.css *.html *.js *.png *.map *.md5 index.{hhc,hhk,hhp} )

# DOXY_ARGS = -d commentscan

all: envtool.chm check_msc

envtool.chm: Makefile envtool.Doxyfile main_page.dox envtool.css ../src/*.[ch]
	rm -f output/msc*.png                               ; \
	doxygen $(DOXY_ARGS) envtool.Doxyfile > doxygen.log ; \
	cd output                                           ; \
	hhc index.hhp                                       ; \
	mv -f envtool.chm ..
	@echo 'Doxygen done. Look in "$(realpath doxygen.log)" for messages.'

check_msc:
ifeq ($(wildcard output/msc*.png),)
	@echo 'MSCGEN failed.'
else
	@echo 'MSCGEN run seemed okay.'
endif

clean:
	rm -fr output
	rm -f $(DOC_GENERATED)
	@echo '"clean" done.'
