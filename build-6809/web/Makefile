
all : index.html manual.html

index.html : main.web mkweb.pl stable-test beta-test
	perl mkweb.pl < main.web > index.html

manual.html : manual.web mkweb.pl
	perl mkweb.pl < manual.web > manual.html
