Word2Neighborhood
====

`Word2Neighborhood` is a simple tool to create dictionary and context data (aka a Neighborhood, aka a Context Vector-like structure) from a corpus file.

If you use this software, please cite:
* [Marco Giorgini](http://www.marcogiorgini.com)

The source code in this repository is provided under the terms of the [Apache License, Version 2.0](http://www.apache.org/licenses/LICENSE-2.0.html).

## Information

`Word2Neighborhood` builds dictionary/neighborhood files from a single corpus file.
`Corpus file` can be a normal text file, or a `CoNLL-U` file, ANSI or UTF-8.

When creating a dictionary from a corpus file it's possible to use a stopword list and/or standard filters (skip words with just digits or punctuations) and/or a filter based on POS (if you work with CoNLL-U files). You can also ask to add bigrams in the dictionary (that will be used, if found, for context extraction).

It's possible to specify a custom radius form context extraction and/or to select the desired size for Neighborhood dimension.

Dictionary file is created with TFxIDF for each element and it has an automatic cut for unprobable items.

## Compiling and using `Word2Neighborhood`

`Word2Neighborhood` is a single C source file, and it should be compiled by any standard C99 compiler.

To create a dictionary from a corpus:

    Word2Neighborhood -corpus <corpusfile> -create dictionary -dict dictionary.txt -stop stopwords.txt

To create a neighborhood file from a corpus (using an already created dictionary file):
	
	Word2Neighborhood -corpus <corpusfile> -create neighborhood -neighbors neighbors.txt -dict dictionary.txt 

## Acknowledgements

This tool is somehow inspired by `Word2Vec` but it doesn't use neural networks to create a compact way to store/recall data. 
It instead uses a mix between a normal and an associative array and it's able to extract context from a multi-GB file, for multimillion words dictionary, using relatively few memory and with a relatively high speed.

