
default:	    all
	
all:		    css/main.css css/base.css css/pages.css js/index.js
	
css/main.css:	    css/main.less
		    lessc css/main.less > css/main.css
	
css/base.css:	    css/base.less
		    lessc css/base.less > css/base.css
	
css/pages.css:	    css/pages.less
		    lessc css/pages.less > css/pages.css

js/index.js:	    js/index.coffee
		    coffee js/index.coffee
