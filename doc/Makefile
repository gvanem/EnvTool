#
# This is a GNU-make Makefile for Envtool's './doc'
#
DOC_GENERATED    = envtool.chm envtool.chw doxygen.log
OUTPUT_GENERATED = $(addprefix ./output/, \
                     *.css *.html *.js *.png *.map *.md5 index.{hhc,hhk,hhp} )

all: envtool.chm

envtool.chm: envtool.Doxyfile main_page.dox envtool.css ../src/*.[ch]
	doxygen envtool.Doxyfile > doxygen.log ; \
	cd output                              ; \
	hhc index.hhp                          ; \
	mv -f envtool.chm ..
	echo 'Doxygen done. Look in "./doxygen.log" for messages.'

clean:
	rm -f $(OUTPUT_GENERATED)
	rm -f $(DOC_GENERATED)
	-rmdir output
	@echo '"clean" done.'
