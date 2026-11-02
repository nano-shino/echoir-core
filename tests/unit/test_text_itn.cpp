#include <string>

#include "doctest.h"
#include "text/itn.h"

using echoir::text::normalize_numbers;

TEST_CASE("normalize_numbers turns ordinals from tenth up into digits with a suffix") {
    CHECK(normalize_numbers("October thirtieth") == "October 30th");
    CHECK(normalize_numbers("the twenty-first") == "the 21st");
    CHECK(normalize_numbers("on the eleventh") == "on the 11th");
    CHECK(normalize_numbers("the twenty-second") == "the 22nd");
    CHECK(normalize_numbers("the thirty-third") == "the 33rd");
    CHECK(normalize_numbers("the hundredth") ==
          "the hundredth");  // a lone "hundredth" has no count
    CHECK(normalize_numbers("one hundredth") == "100th");
}

TEST_CASE("normalize_numbers leaves simple ordinals as words") {
    CHECK(normalize_numbers("the second biggest drop") == "the second biggest drop");
    CHECK(normalize_numbers("first of all") == "first of all");
}

TEST_CASE("normalize_numbers turns decimals into digits") {
    CHECK(normalize_numbers("four point five") == "4.5");
    CHECK(normalize_numbers("four-point-five") == "4.5");
    CHECK(normalize_numbers("three point two one") == "3.21");
    CHECK(normalize_numbers("zero point five") == "0.5");
    // "zero" alone stays a word.
    CHECK(normalize_numbers("zero") == "zero");
}

TEST_CASE("normalize_numbers turns cardinals from ten up into digits") {
    CHECK(normalize_numbers("ten") == "10");
    CHECK(normalize_numbers("twenty three") == "23");
    CHECK(normalize_numbers("thirty-nine") == "39");
    CHECK(normalize_numbers("one hundred five") == "105");
    CHECK(normalize_numbers("two thousand five hundred") == "2500");
    CHECK(normalize_numbers("one hundred thousand") == "100000");
}

TEST_CASE("normalize_numbers keeps single digits as words, as prose does") {
    CHECK(normalize_numbers("two months") == "two months");
    CHECK(normalize_numbers("three decades") == "three decades");
    CHECK(normalize_numbers("one or both") == "one or both");
    CHECK(normalize_numbers("nine") == "nine");
}

TEST_CASE("normalize_numbers attaches percent and dollars to the figure") {
    CHECK(normalize_numbers("thirty nine percent") == "39%");
    CHECK(normalize_numbers("five percent") == "5%");
    CHECK(normalize_numbers("seventy seven billion dollars") == "$77 billion");
    CHECK(normalize_numbers("six point three billion dollars") == "$6.3 billion");
    CHECK(normalize_numbers("forty two point four billion dollars") == "$42.4 billion");
    CHECK(normalize_numbers("five hundred billion dollars") == "$500 billion");
    CHECK(normalize_numbers("six dollars") == "$6");
}

TEST_CASE("normalize_numbers writes a million or billion as digits and a unit word") {
    CHECK(normalize_numbers("one million") == "1 million");
    // Two scale words: a decimal of the larger one, while it needs at most three places.
    CHECK(normalize_numbers("one billion five hundred million") == "1.5 billion");
    CHECK(normalize_numbers("two million five thousand") == "2.005 million");
    CHECK(normalize_numbers("two million five thousand one") == "2005001");
}

TEST_CASE("normalize_numbers keeps the text around a number unchanged") {
    CHECK(normalize_numbers("It's Friday, October thirtieth. Well,") ==
          "It's Friday, October 30th. Well,");
    CHECK(normalize_numbers("up thirty nine percent!") == "up 39%!");
    CHECK(normalize_numbers("build-out and exchange-traded") == "build-out and exchange-traded");
    CHECK(normalize_numbers("") == "");
    CHECK(normalize_numbers("no numbers here") == "no numbers here");
}

TEST_CASE("normalize_numbers does not join separate numbers or words that only look like numbers") {
    // "five six" is two numbers, and "five" alone stays a word.
    CHECK(normalize_numbers("five six") == "five six");
    // "point" needs digit words after it, so this is not a decimal.
    CHECK(normalize_numbers("one point of view") == "one point of view");
    // A hundred or a billion alone has no count in front of it.
    CHECK(normalize_numbers("a hundred people") == "a hundred people");
    // Words that contain number words are not numbers.
    CHECK(normalize_numbers("tennis twenties") == "tennis twenties");
}

TEST_CASE("normalize_numbers converts a spoken paragraph with dates, percents and money") {
    CHECK(normalize_numbers(
              "It's Friday, October thirtieth. Well, it's a quiet weekend, and the team "
              "has taken a break.") ==
          "It's Friday, October 30th. Well, it's a quiet weekend, and the team "
          "has taken a break.");
    CHECK(normalize_numbers("revenue there was up thirty nine percent.") ==
          "revenue there was up 39%.");
    CHECK(normalize_numbers("the company spent forty two point four billion dollars in cash.") ==
          "the company spent $42.4 billion in cash.");
    CHECK(normalize_numbers("It announced more than seventy seven billion dollars of spending") ==
          "It announced more than $77 billion of spending");
    CHECK(normalize_numbers("Those shares dropped twenty three percent after hours") ==
          "Those shares dropped 23% after hours");
    CHECK(normalize_numbers("it lost six point three billion dollars.") == "it lost $6.3 billion.");
    CHECK(normalize_numbers("Its market value fell five hundred billion dollars.") ==
          "Its market value fell $500 billion.");
    // Small counts and simple ordinals stay words.
    CHECK(normalize_numbers("over the past two months") == "over the past two months");
    CHECK(normalize_numbers("the second biggest drop") == "the second biggest drop");
    CHECK(normalize_numbers("for nearly three decades") == "for nearly three decades");
}

TEST_CASE("normalize_numbers reads \"and\", \"a hundred\" and hundreds of a teen") {
    CHECK(normalize_numbers("two hundred and forty thousand") == "240000");
    CHECK(normalize_numbers("nine hundred and ninety-nine") == "999");
    CHECK(normalize_numbers("one thousand and five") == "1005");
    CHECK(normalize_numbers("a hundred and fifty") == "150");
    CHECK(normalize_numbers("a thousand five hundred") == "1500");
    CHECK(normalize_numbers("twelve hundred") == "1200");
    CHECK(normalize_numbers("twenty-five hundred dollars") == "$2500");
    CHECK(normalize_numbers("one hundred and one dalmatians") == "101 dalmatians");
    CHECK(normalize_numbers("one thousand and first") == "1001st");
    CHECK(normalize_numbers("the hundred and fiftieth anniversary") == "the 150th anniversary");
    // An "and" that does not continue the number is a word.
    CHECK(normalize_numbers("between ten and twenty") == "between 10 and 20");
    CHECK(normalize_numbers("two hundred and five hundred") == "200 and 500");
    CHECK(normalize_numbers("a hundred thousand and five hundred thousand") == "100000 and 500000");
    // Loose speech puts "and" before a whole group too.
    CHECK(normalize_numbers("two thousand and eight hundred") == "2800");
    CHECK(normalize_numbers("three million and five hundred thousand") == "3.5 million");
    // "a hundred" alone stays words, as prose writes it, and so does the fraction "a tenth".
    CHECK(normalize_numbers("a hundred people") == "a hundred people");
    CHECK(normalize_numbers("a tenth of a second") == "a tenth of a second");
    CHECK(normalize_numbers("I need a tenth.") == "I need a tenth.");
    CHECK(normalize_numbers("one millionth of a second") == "one millionth of a second");
}

TEST_CASE("normalize_numbers reads years and decades") {
    CHECK(normalize_numbers("in nineteen ninety nine") == "in 1999");
    CHECK(normalize_numbers("twenty twenty-four") == "2024");
    CHECK(normalize_numbers("nineteen oh five") == "1905");
    CHECK(normalize_numbers("two thousand and eight") == "2008");
    CHECK(normalize_numbers("nineteen hundred and forty five") == "1945");
    CHECK(normalize_numbers("the nineteen eighties") == "the 1980s");
}

TEST_CASE("normalize_numbers reads clock times only with a.m., p.m. or \"at\"") {
    CHECK(normalize_numbers("nine forty-five a.m.") == "9:45 a.m.");
    CHECK(normalize_numbers("seven oh five pm") == "7:05 pm");
    CHECK(normalize_numbers("nine a.m.") == "9 a.m.");
    CHECK(normalize_numbers("at eleven thirty") == "at 11:30");
    CHECK(normalize_numbers("the twelve fifteen-year-olds") == "the 12 15-year-olds");
}

TEST_CASE("normalize_numbers reads decimals, fractions, money and signs") {
    CHECK(normalize_numbers("three point fourteen") == "3.14");
    CHECK(normalize_numbers("zero point zero five percent") == "0.05%");
    CHECK(normalize_numbers("point five percent") == "0.5%");
    CHECK(normalize_numbers("zero percent") == "0%");
    CHECK(normalize_numbers("ten per cent") == "10%");
    CHECK(normalize_numbers("two point five thousand") == "2500");
    CHECK(normalize_numbers("one and a half million dollars") == "$1.5 million");
    CHECK(normalize_numbers("twelve and a half percent") == "12.5%");
    CHECK(normalize_numbers("two and a half years") == "two and a half years");
    CHECK(normalize_numbers("five dollars and fifty cents") == "$5.50");
    CHECK(normalize_numbers("a million dollars") == "$1 million");
    CHECK(normalize_numbers("one dollar") == "$1");
    CHECK(normalize_numbers("a dollar") == "a dollar");
    CHECK(normalize_numbers("a ten-dollar bill") == "a $10 bill");
    CHECK(normalize_numbers("a sixteen billion-dollar deal") == "a $16 billion deal");
    CHECK(normalize_numbers("minus twelve degrees") == "-12 degrees");
    CHECK(normalize_numbers("negative two percent") == "-2%");
    CHECK(normalize_numbers("five minus three") == "five minus three");
    // A sign makes a figure of a small number too, but "negative one" can be prose.
    CHECK(normalize_numbers("fell to minus five degrees") == "fell to -5 degrees");
    CHECK(normalize_numbers("came back negative one time") == "came back negative one time");
    // A year-shaped pair before "dollars" is a price.
    CHECK(normalize_numbers("it costs nineteen ninety nine dollars") == "it costs $19.99");
    // With a singular "dollar" the article belongs to the bill.
    CHECK(normalize_numbers("A hundred dollar bill fell out") == "A $100 bill fell out");
    CHECK(normalize_numbers("a hundred dollars") == "$100");
}

TEST_CASE("normalize_numbers tolerates loose speech: spacing, fillers and dashes") {
    CHECK(normalize_numbers("twenty  five") == "25");
    CHECK(normalize_numbers("twenty\tfive") == "25");
    CHECK(normalize_numbers("thirty\xC2\xA0nine percent") == "39%");  // no-break space
    CHECK(normalize_numbers("thirty\xE2\x80\x91nine") == "39");       // no-break hyphen
    CHECK(normalize_numbers("twenty\xE2\x80\x93"
                            "five percent") == "25%");  // en dash
    CHECK(normalize_numbers("ten\xE2\x80\x93twenty") ==
          "10\xE2\x80\x93"
          "20");  // a range
    CHECK(normalize_numbers("two hundred uh forty thousand") == "240000");
    CHECK(normalize_numbers("um twenty uh five dollars") == "um $25");
    CHECK(normalize_numbers("I said uh, twenty") == "I said uh, 20");
    CHECK(normalize_numbers("twenty\xE2\x80\x82"
                            "five") == "25");  // en space
    // Letters glued to digits or accented letters are part of another token.
    CHECK(normalize_numbers("twenty5 1twenty") == "twenty5 1twenty");
    CHECK(normalize_numbers("\xC3\xA9ten twenty\xC3\xA9") == "\xC3\xA9ten twenty\xC3\xA9");
    // A comma ends a number: "two hundred, forty" may be a list.
    CHECK(normalize_numbers("two hundred, uh, forty") == "200, uh, 40");
}

TEST_CASE("normalize_numbers leaves names with capitalized number words alone") {
    CHECK(normalize_numbers("the Big Ten") == "the Big Ten");
    CHECK(normalize_numbers("Ocean's Eleven") == "Ocean's Eleven");
    CHECK(normalize_numbers("Seven-Eleven store") == "Seven-Eleven store");
    // At the start of a sentence a capital is just a capital.
    CHECK(normalize_numbers("Twenty people came. Thirty left.") == "20 people came. 30 left.");
    CHECK(normalize_numbers("THIRTY-NINE") == "39");
}
