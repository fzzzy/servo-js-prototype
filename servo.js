
function cb() {
    print("callback", document, document.body, document.body.firstChild.data);
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
let url = yield receive("url");
xhr.open("GET", url);
xhr.send("");
