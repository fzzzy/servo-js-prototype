
function mutation(evt) {
    var domjsNodeStr = evt.child;
    if (domjsNodeStr === undefined) {
        if (evt.type === 2) {
            print(evt.target, "Attr", evt.name, evt.value);
        } else if (evt.type === 1) {
            print(evt.target, "Value", JSON.stringify(evt.data));
        }
        return;
    }

    var NULL = '\0';

    switch (domjsNodeStr.charAt(0)) {
      case 'T':
        print(evt.nid, "Text", JSON.stringify(
            domjsNodeStr.substr(1).split(NULL)[0]).substr(1, domjsNodeStr.length - 2)
        );
        break;
      case 'C':
        print(evt.nid, "Comment node", JSON.stringify(
            domjsNodeStr.substr(1).split(NULL)[0])
        );
        break;
      case 'H':
          // html node
      case 'E':
        var spl = domjsNodeStr.substr(1).split(NULL);
        print(evt.nid, "Element", spl[0]);
        break;
    case 'D':
        var spl = domjsNodeStr.substr(1).split(NULL);
        print(evt.nid, "Document node", JSON.stringify(spl[0]));
        break;
      default:
        throw new Error('Unhandled case of stringified node: ' + domjsNodeStr.charAt(0));
    }
}

let $ = window.jQuery;

function cb() {
    print("callback", document, document.body);
    $("a").addClass("test");
    $("script").each(function() {
        //print("hi", this.getAttribute('src'));
    });
}

document.implementation.mozSetOutputMutationHandler(document, mutation);

let xhr = new XMLHttpRequest();

xhr.onreadystatechange = function() {
    if (this.readyState === 4) {
        var newdoc = document.implementation.mozHTMLParser(mutation).end(this.responseText);
        print(newdoc);
        //window.parseHtmlDocument(this.responseText, document, cb, null);
    }
}

let url = yield receive("url");
print("Loading", url);

spawn('foo.js', 1);
let res = yield receive('spawn');
let address = actors[res];
address('foo', "what's happenin!");

xhr.open("GET", url);
xhr.send("");

