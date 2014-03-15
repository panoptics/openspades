
default:	    all
	
all:		    css/main.css
	
css/main.css:	    css/main.less
		    lessc css/main.less > css/main.css

