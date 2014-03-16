<?php

$body = null;
$options = null;

function begin_body($opt) {
    global $options;
    
    $options = $opt;
    ob_start();
}

function end_body() {
    global $body;
    
    $body = ob_get_clean();
    ob_end_clean();
}

function finalize_page() {
    global $body;
    global $options;
    if($body === null ||
       $options === null) {
        die("Body not provided");
    }
    ?>
<!DOCTYPE html>
<html>
    <head>
        <title><?php print $options['title']; ?> - OpenSpades</title>
        <meta http-equiv="Content-Type" content="text/html; charset=UTF-8">
        <link rel="stylesheet" type="text/css" href="css/base.css">
        <link rel="stylesheet" type="text/css" href="css/pages.css">
    </head>
    <body>
        <header id="header-wrapper">
            <div id="header">
            </div>
        </header>
        <nav id="global-nav-wrapper">
            <div id="global-nav">
                <ul>
                    <li><a href="/">Top</a></li>
                    <li><a href="/">Media</a></li>
                    <li><a href="/">Community</a></li>
                    <li><a href="/">Development</a></li>
                    <li class="download"><a href="/download.php">Download Now!</a></li>
                </ul>
            </div>
        </nav>
        <main>
            <?php print $body; ?>
        </main>
    </body>
</html>
<?php
    
}

