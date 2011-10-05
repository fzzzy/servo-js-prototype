
let $ = window.jQuery;

function cb() {
    print("callback", document, document.body);
    $("a").addClass("test");
    $("a").each(function() {
        print("hi", this.getAttribute('href'));
    });

test("a basic test example", function() {
  ok( true, "this test is fine" );
  var value = "hello";
  equal( value, "hello", "We expect value to be hello" );
});

module("Module A");

test("first test within module", function() {
  ok( true, "all pass" );
});

test("second test within module", function() {
  ok( true, "all pass" );
});

module("Module B");

test("some other test", function() {
  expect(2);
  equal( true, false, "failing test" );
  equal( true, true, "passing test" );
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
print("QUNIT", window.QUnit);

let url = "http://localhost:80/";
xhr.open("GET", url);
xhr.send("");

