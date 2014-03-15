
default:	    all
	
all:		    css/main.css js/index.js
	
css/main.css:	    css/main.less
		    lessc css/main.less > css/main.css

js/index.js:	    js/index.coffee
		    coffee js/index.coffee
