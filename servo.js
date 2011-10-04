
let $ = window.jQuery;

function cb() {
    print("callback", document, document.body);
    $("a").addClass("test");
    $("a").each(function() {
        print("hi", this.getAttribute('href'));
    });
}

function mutation(evt) {
    print("mutation", JSON.stringify(evt));
}

document.implementation.mozSetOutputMutationHandler(document, mutation);

let xhr = new XMLHttpRequest();

xhr.onreadystatechange = function() {
    if (this.readyState === 4) {
        window.parseHtmlDocument(this.responseText, document, cb, null);
    }
}
 
print("JQUERY", window.jQuery);

let url = "http://localhost:80/";
xhr.open("GET", url);
xhr.send("");
