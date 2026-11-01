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
    // A number starts with a unit, teen or ten word, so a leading "zero" stays a word.
    CHECK(normalize_numbers("zero point five") == "zero point five");
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
    // Two scale words: the full value, since "1500 million" is not how the figure is written.
    CHECK(normalize_numbers("one billion five hundred million") == "1500000000");
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
