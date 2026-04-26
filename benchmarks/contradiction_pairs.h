/*
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 * By contributing to this project, you agree to license your contributions
 * under the GPLv3 (or any later version) or any future licenses chosen by
 * the project author(s).
 *
 * Curated fact-pair dataset for contradiction detection experiments.
 *
 * 210+ labeled pairs across 5 relationship types and 15 semantic
 * categories.  Each pair is hand-labeled.  The dataset is designed
 * for statistical significance (30+ pairs per label) and includes
 * adversarial pairs at the decision boundary.
 *
 * Labels:
 *   contradiction — same subject+attribute, incompatible values
 *   paraphrase    — same meaning, different wording
 *   identical     — same meaning, near-identical wording
 *   related       — same topic, compatible/additive information
 *   unrelated     — different topics entirely
 */

#ifndef CONTRADICTION_PAIRS_H
#define CONTRADICTION_PAIRS_H

typedef struct {
   const char *fact_a;
   const char *fact_b;
   const char *label;
   const char *category;
} fact_pair_t;

static const fact_pair_t FACT_PAIRS[] = {

   /* ================================================================
    * CONTRADICTIONS (80 pairs)
    * Same subject+attribute, incompatible values.
    * ================================================================ */

   /* ── employment (8) ── */
   { "Alice works at Google as a software engineer",
     "Alice works at Microsoft as a software engineer", "contradiction", "employment" },
   { "Bob is a teacher at Lincoln High School", "Bob is a teacher at Roosevelt High School",
     "contradiction", "employment" },
   { "User works as a data scientist at Amazon", "User works as a data scientist at Meta",
     "contradiction", "employment" },
   { "Sarah is the CEO of a startup", "Sarah is a junior analyst at a bank", "contradiction",
     "employment" },
   { "David works in marketing at Nike", "David works in marketing at Adidas", "contradiction",
     "employment" },
   { "User is a freelance graphic designer", "User is a full-time nurse at City Hospital",
     "contradiction", "employment" },
   { "Tom is a pilot for Delta Airlines", "Tom is a pilot for United Airlines", "contradiction",
     "employment" },
   { "User's manager is named Rachel", "User's manager is named Kevin", "contradiction",
     "employment" },

   /* ── location (7) ── */
   { "User lives in New York City", "User lives in San Francisco", "contradiction", "location" },
   { "Bob lives in London, England", "Bob lives in Paris, France", "contradiction", "location" },
   { "Alice grew up in Chicago", "Alice grew up in Houston", "contradiction", "location" },
   { "User's apartment is in Brooklyn", "User's apartment is in Manhattan", "contradiction",
     "location" },
   { "The family lives in Toronto, Canada", "The family lives in Sydney, Australia",
     "contradiction", "location" },
   { "User moved to Seattle last year", "User moved to Denver last year", "contradiction",
     "location" },
   { "Sarah's office is on the 5th floor", "Sarah's office is on the 12th floor", "contradiction",
     "location" },

   /* ── relationship (6) ── */
   { "Bob is married to Carol", "Bob is married to Diana", "contradiction", "relationship" },
   { "User is dating someone named Alex", "User is dating someone named Jordan", "contradiction",
     "relationship" },
   { "Alice's best friend is named Maria", "Alice's best friend is named Sophie", "contradiction",
     "relationship" },
   { "User is single and not dating anyone", "User is in a long-term relationship", "contradiction",
     "relationship" },
   { "Tom's brother is named James", "Tom's brother is named Michael", "contradiction",
     "relationship" },
   { "User's roommate is named Chris", "User's roommate is named Pat", "contradiction",
     "relationship" },

   /* ── preference (6) ── */
   { "User prefers coffee over tea", "User prefers tea over coffee", "contradiction",
     "preference" },
   { "Alice's favorite movie is The Godfather", "Alice's favorite movie is Pulp Fiction",
     "contradiction", "preference" },
   { "User prefers Android phones over iPhones", "User prefers iPhones over Android phones",
     "contradiction", "preference" },
   { "Bob likes summer more than winter", "Bob likes winter more than summer", "contradiction",
     "preference" },
   { "User's favorite programming language is Python",
     "User's favorite programming language is Rust", "contradiction", "preference" },
   { "Sarah prefers working from home", "Sarah prefers working from the office", "contradiction",
     "preference" },

   /* ── negation (8) ── */
   { "User likes cats", "User doesn't like cats", "contradiction", "negation" },
   { "Sarah enjoys swimming", "Sarah hates swimming", "contradiction", "negation" },
   { "Bob is interested in politics", "Bob has no interest in politics", "contradiction",
     "negation" },
   { "User loves cooking", "User never cooks", "contradiction", "negation" },
   { "Alice drinks alcohol socially", "Alice doesn't drink alcohol at all", "contradiction",
     "negation" },
   { "User enjoys public speaking", "User dreads public speaking", "contradiction", "negation" },
   { "Tom is comfortable flying", "Tom is terrified of flying", "contradiction", "negation" },
   { "User finds math easy", "User struggles with math", "contradiction", "negation" },

   /* ── quantity (5) ── */
   { "User has two children", "User has three children", "contradiction", "quantity" },
   { "Alice has one cat", "Alice has four cats", "contradiction", "quantity" },
   { "Bob has been married once", "Bob has been married three times", "contradiction", "quantity" },
   { "User speaks two languages", "User speaks five languages", "contradiction", "quantity" },
   { "The family has one car", "The family has three cars", "contradiction", "quantity" },

   /* ── temporal/status (6) ── */
   { "User is currently a student at MIT", "User graduated from MIT last year", "contradiction",
     "temporal" },
   { "Alice is pregnant", "Alice gave birth last month", "contradiction", "temporal" },
   { "Bob is looking for a new job", "Bob just started a new job last week", "contradiction",
     "temporal" },
   { "User is learning to drive", "User has had a driver's license for ten years", "contradiction",
     "temporal" },
   { "Sarah is planning her wedding", "Sarah got married last spring", "contradiction",
     "temporal" },
   { "User is renting an apartment", "User bought a house recently", "contradiction", "temporal" },

   /* ── dietary/lifestyle (5) ── */
   { "User is vegetarian", "User eats meat regularly", "contradiction", "dietary" },
   { "Alice follows a keto diet", "Alice follows a vegan diet", "contradiction", "dietary" },
   { "Bob is a smoker", "Bob has never smoked", "contradiction", "dietary" },
   { "User exercises every day", "User hasn't exercised in months", "contradiction", "dietary" },
   { "Sarah drinks coffee every morning", "Sarah has quit caffeine entirely", "contradiction",
     "dietary" },

   /* ── skill/ability (5) ── */
   { "User speaks French fluently", "User doesn't speak any French", "contradiction", "skill" },
   { "Alice can play the piano", "Alice cannot play any musical instrument", "contradiction",
     "skill" },
   { "Bob is an expert at chess", "Bob doesn't know how to play chess", "contradiction", "skill" },
   { "User knows how to code in Java", "User has never written any code", "contradiction",
     "skill" },
   { "Tom is a certified scuba diver", "Tom can't swim", "contradiction", "skill" },

   /* ── age/identity (5) ── */
   { "User is 32 years old", "User is 45 years old", "contradiction", "age" },
   { "Alice was born in 1990", "Alice was born in 1985", "contradiction", "age" },
   { "Bob is left-handed", "Bob is right-handed", "contradiction", "age" },
   { "User has brown eyes", "User has blue eyes", "contradiction", "age" },
   { "Sarah is 5 feet 4 inches tall", "Sarah is 5 feet 10 inches tall", "contradiction", "age" },

   /* ── education (5) ── */
   { "User studied computer science at MIT", "User studied computer science at Stanford",
     "contradiction", "education" },
   { "Alice has a PhD in biology", "Alice has a PhD in chemistry", "contradiction", "education" },
   { "Bob graduated from Harvard Law School", "Bob graduated from Yale Law School", "contradiction",
     "education" },
   { "User dropped out of college", "User has a master's degree", "contradiction", "education" },
   { "Tom went to public school", "Tom was homeschooled", "contradiction", "education" },

   /* ── vehicle/possession (5) ── */
   { "User drives a Toyota Camry", "User drives a Honda Civic", "contradiction", "vehicle" },
   { "Alice owns a house in the suburbs", "Alice rents a studio apartment downtown",
     "contradiction", "vehicle" },
   { "Bob rides a Harley-Davidson motorcycle", "Bob rides a Ducati motorcycle", "contradiction",
     "vehicle" },
   { "User has an iPhone 15", "User has a Samsung Galaxy S24", "contradiction", "vehicle" },
   { "The family drives a minivan", "The family drives an SUV", "contradiction", "vehicle" },

   /* ── pet (4) ── */
   { "User has a dog named Max", "User has no pets", "contradiction", "pet" },
   { "Alice's cat is a Maine Coon", "Alice's cat is a Siamese", "contradiction", "pet" },
   { "Bob has a golden retriever", "Bob has a German shepherd", "contradiction", "pet" },
   { "User is allergic to dogs", "User owns three dogs", "contradiction", "pet" },

   /* ── contact (5) ── */
   { "User's email is bob@gmail.com", "User's email is bob@yahoo.com", "contradiction", "contact" },
   { "Alice's phone number starts with 555-012", "Alice's phone number starts with 555-098",
     "contradiction", "contact" },
   { "User's birthday is March 15", "User's birthday is June 22", "contradiction", "contact" },
   { "Bob goes by the nickname Bear", "Bob goes by the nickname Chip", "contradiction", "contact" },
   { "User's home timezone is Eastern", "User's home timezone is Pacific", "contradiction",
     "contact" },

   /* ── hobby/interest (5) ── */
   { "User loves running marathons", "User hates running", "contradiction", "hobby" },
   { "Alice plays tennis every weekend", "Alice has never played tennis", "contradiction",
     "hobby" },
   { "Bob is an avid reader", "Bob rarely reads books", "contradiction", "hobby" },
   { "User's hobby is painting watercolors", "User's hobby is woodworking", "contradiction",
     "hobby" },
   { "Sarah is passionate about gardening", "Sarah has no interest in gardening", "contradiction",
     "hobby" },

   /* ================================================================
    * PARAPHRASES (40 pairs)
    * Same meaning, different wording.
    * ================================================================ */

   /* employment */
   { "Alice works at Google", "Alice is employed by Google", "paraphrase", "employment" },
   { "Bob is a teacher", "Bob teaches for a living", "paraphrase", "employment" },
   { "User works as a software engineer", "User is a software developer by profession",
     "paraphrase", "employment" },
   { "Sarah is the head of marketing", "Sarah leads the marketing department", "paraphrase",
     "employment" },
   { "Tom got fired from his job", "Tom was let go from his position", "paraphrase", "employment" },

   /* location */
   { "Bob lives in New York City", "Bob resides in NYC", "paraphrase", "location" },
   { "User is from California", "User grew up in California", "paraphrase", "location" },
   { "Alice moved to London last year", "Alice relocated to London a year ago", "paraphrase",
     "location" },
   { "The office is in downtown Seattle", "The office is located in central Seattle", "paraphrase",
     "location" },

   /* relationship */
   { "User is married to Jane", "Jane is User's wife", "paraphrase", "relationship" },
   { "Bob and Carol are engaged", "Carol and Bob are planning to get married", "paraphrase",
     "relationship" },
   { "Alice has two siblings", "Alice has a brother and a sister", "paraphrase", "relationship" },
   { "Tom is a father of three", "Tom has three kids", "paraphrase", "relationship" },

   /* preference */
   { "User prefers coffee over tea", "User likes coffee more than tea", "paraphrase",
     "preference" },
   { "Alice's favorite color is blue", "Alice likes blue the best", "paraphrase", "preference" },
   { "Bob hates spicy food", "Bob can't stand spicy food", "paraphrase", "preference" },

   /* dietary */
   { "User is vegetarian", "User doesn't eat meat", "paraphrase", "dietary" },
   { "Alice avoids gluten", "Alice follows a gluten-free diet", "paraphrase", "dietary" },
   { "Bob quit drinking alcohol", "Bob stopped consuming alcohol", "paraphrase", "dietary" },

   /* skill */
   { "User speaks French fluently", "User is fluent in the French language", "paraphrase",
     "skill" },
   { "Alice can play the piano well", "Alice is a skilled pianist", "paraphrase", "skill" },
   { "Bob knows JavaScript and Python", "Bob can program in JavaScript and Python", "paraphrase",
     "skill" },

   /* education */
   { "User has a degree in computer science", "User studied computer science in college",
     "paraphrase", "education" },
   { "Alice graduated from MIT", "Alice got her degree from MIT", "paraphrase", "education" },

   /* age */
   { "User is 30 years old", "User is thirty", "paraphrase", "age" },
   { "Alice was born in 1992", "Alice's birth year is 1992", "paraphrase", "age" },

   /* hobby */
   { "User loves hiking", "User enjoys going on hikes", "paraphrase", "hobby" },
   { "Bob plays guitar in a band", "Bob is a guitarist in a band", "paraphrase", "hobby" },
   { "Alice jogs every morning", "Alice goes for a run each morning", "paraphrase", "hobby" },

   /* pet */
   { "User has a dog named Max", "User's dog is called Max", "paraphrase", "pet" },
   { "Alice owns two cats", "Alice has a pair of cats", "paraphrase", "pet" },

   /* vehicle */
   { "User drives a Toyota", "User's car is a Toyota", "paraphrase", "vehicle" },
   { "Bob rides his bike to work", "Bob cycles to the office", "paraphrase", "vehicle" },

   /* contact */
   { "User's birthday is in March", "User was born in March", "paraphrase", "contact" },

   /* temporal */
   { "User recently got promoted", "User received a promotion not long ago", "paraphrase",
     "temporal" },
   { "Alice just had a baby", "Alice recently gave birth", "paraphrase", "temporal" },

   /* quantity */
   { "User has two kids", "User is a parent of two", "paraphrase", "quantity" },

   /* low-overlap paraphrases (adversarial — few shared words) */
   { "Bob works at Google", "Google employs Bob as a member of their staff", "paraphrase",
     "employment" },
   { "User doesn't eat meat", "Consuming animal flesh is something User avoids", "paraphrase",
     "dietary" },
   { "Alice lives in Tokyo", "The Japanese capital is where Alice resides", "paraphrase",
     "location" },

   /* ================================================================
    * IDENTICAL (20 pairs)
    * Near-identical wording, trivial variation.
    * ================================================================ */

   { "Bob likes pizza", "Bob likes pizza a lot", "identical", "preference" },
   { "User has a cat", "User has a cat", "identical", "pet" },
   { "Alice works at Google", "Alice works at Google", "identical", "employment" },
   { "User lives in New York", "User lives in New York City", "identical", "location" },
   { "Bob is married to Carol", "Bob is married to Carol", "identical", "relationship" },
   { "User is 30 years old", "User is 30", "identical", "age" },
   { "Alice has two children", "Alice has 2 children", "identical", "quantity" },
   { "User drives a Honda Civic", "User drives a Honda Civic", "identical", "vehicle" },
   { "Bob speaks Spanish", "Bob speaks Spanish", "identical", "skill" },
   { "User's email is test@gmail.com", "User's email is test@gmail.com", "identical", "contact" },
   { "Alice is a nurse", "Alice is a nurse", "identical", "employment" },
   { "User likes chocolate ice cream", "User likes chocolate ice cream", "identical",
     "preference" },
   { "Bob has a dog named Rex", "Bob has a dog named Rex", "identical", "pet" },
   { "User is vegetarian", "User is a vegetarian", "identical", "dietary" },
   { "Alice studied at Harvard", "Alice studied at Harvard", "identical", "education" },
   { "User was born in 1990", "User was born in 1990", "identical", "age" },
   { "Bob lives in Chicago", "Bob lives in Chicago, Illinois", "identical", "location" },
   { "User is afraid of heights", "User is scared of heights", "identical", "hobby" },
   { "Sarah has three siblings", "Sarah has 3 siblings", "identical", "relationship" },
   { "Tom is a morning person", "Tom is a morning person", "identical", "preference" },

   /* ================================================================
    * RELATED (40 pairs)
    * Same topic, compatible/additive — NOT contradictions.
    * ================================================================ */

   /* additive facts (both can be true) */
   { "User has a cat", "User's cat is named Luna", "related", "pet" },
   { "User likes Italian food", "User likes Thai food", "related", "preference" },
   { "Alice works at Google", "Alice works on the search team at Google", "related", "employment" },
   { "Bob lives in New York", "Bob's apartment is near Central Park", "related", "location" },
   { "User has two children", "User's oldest child is named Emma", "related", "relationship" },
   { "Sarah speaks French", "Sarah also speaks German", "related", "skill" },
   { "User drives a Toyota Camry", "User's car is silver", "related", "vehicle" },
   { "Alice has a dog", "Alice adopted a second dog recently", "related", "pet" },
   { "Bob studied at MIT", "Bob majored in electrical engineering at MIT", "related", "education" },
   { "User is 30 years old", "User's birthday is in June", "related", "age" },

   /* elaborative facts */
   { "User works as an engineer", "User has been an engineer for 8 years", "related",
     "employment" },
   { "Alice plays tennis", "Alice plays tennis at the community center", "related", "hobby" },
   { "Bob is married", "Bob got married in 2018", "related", "relationship" },
   { "User is vegetarian", "User became vegetarian five years ago", "related", "dietary" },
   { "Sarah lives in London", "Sarah has lived in London since 2020", "related", "location" },
   { "Tom has a cat named Whiskers", "Whiskers is an orange tabby", "related", "pet" },
   { "User speaks Spanish", "User learned Spanish in high school", "related", "skill" },
   { "Alice drives to work", "Alice's commute takes 45 minutes", "related", "vehicle" },
   { "Bob is a runner", "Bob ran the Boston Marathon last year", "related", "hobby" },
   { "User has a PhD", "User defended their PhD thesis in 2019", "related", "education" },

   /* same domain, different aspect */
   { "User likes reading fiction", "User also enjoys reading history books", "related", "hobby" },
   { "Alice drinks coffee every morning", "Alice takes her coffee with oat milk", "related",
     "dietary" },
   { "Bob has two brothers", "Bob's younger brother lives in Texas", "related", "relationship" },
   { "User works from home on Fridays", "User's office is in downtown Portland", "related",
     "employment" },
   { "Sarah is studying for the bar exam", "Sarah went to law school at Georgetown", "related",
     "education" },
   { "Tom lives in a house", "Tom's house has three bedrooms", "related", "location" },
   { "User is 45 years old", "User was born in Ohio", "related", "age" },
   { "Alice has a German shepherd", "Alice walks her dog twice a day", "related", "pet" },
   { "Bob's favorite sport is basketball", "Bob also watches football on Sundays", "related",
     "hobby" },
   { "User's phone number is 555-0123", "User prefers to be contacted by email", "related",
     "contact" },

   /* tricky: same domain, could seem contradictory but aren't */
   { "User works at Google", "User used to work at Microsoft", "related", "employment" },
   { "Alice lives in San Francisco", "Alice grew up in New York", "related", "location" },
   { "Bob is married to Carol", "Bob was previously married to Anne", "related", "relationship" },
   { "User has a Toyota", "User is thinking about buying a Honda", "related", "vehicle" },
   { "Sarah studied biology in college", "Sarah now works in software engineering", "related",
     "education" },
   { "Tom has brown hair", "Tom used to have blond hair as a child", "related", "age" },
   { "User is learning to cook Italian food", "User already knows how to cook Thai food", "related",
     "skill" },
   { "Alice runs 5K races", "Alice is training for a half marathon", "related", "hobby" },
   { "User eats sushi regularly", "User avoids raw oysters", "related", "dietary" },
   { "Bob's birthday is in March", "Bob is turning 40 this year", "related", "contact" },

   /* ================================================================
    * UNRELATED (30 pairs)
    * Different topics entirely.
    * ================================================================ */

   { "User likes hiking on weekends", "Alice is a dentist in Portland", "unrelated",
     "cross_topic" },
   { "Bob has a golden retriever", "Sarah studied chemistry at Oxford", "unrelated",
     "cross_topic" },
   { "User works at Amazon", "Tom's favorite color is green", "unrelated", "cross_topic" },
   { "Alice lives in Tokyo", "Bob drives a Ford truck", "unrelated", "cross_topic" },
   { "User is 28 years old", "Alice plays the violin", "unrelated", "cross_topic" },
   { "Bob is married to Carol", "User's car needs an oil change", "unrelated", "cross_topic" },
   { "Sarah speaks Mandarin", "Tom has two cats", "unrelated", "cross_topic" },
   { "User has a PhD in physics", "Alice prefers tea over coffee", "unrelated", "cross_topic" },
   { "Bob runs every morning", "Sarah's birthday is in December", "unrelated", "cross_topic" },
   { "User is vegetarian", "Alice's office is on the 3rd floor", "unrelated", "cross_topic" },
   { "Tom works as a pilot", "User's dog is named Buddy", "unrelated", "cross_topic" },
   { "Alice has three children", "Bob quit smoking last year", "unrelated", "cross_topic" },
   { "User's email is test@example.com", "Sarah enjoys painting landscapes", "unrelated",
     "cross_topic" },
   { "Bob studied at Yale", "Alice's apartment has a balcony", "unrelated", "cross_topic" },
   { "User prefers working from home", "Tom was born in Canada", "unrelated", "cross_topic" },
   { "Sarah has a cat named Milo", "Bob drives a motorcycle", "unrelated", "cross_topic" },
   { "User is learning guitar", "Alice is allergic to peanuts", "unrelated", "cross_topic" },
   { "Bob lives in Austin, Texas", "User's favorite movie is Inception", "unrelated",
     "cross_topic" },
   { "Alice works as a nurse", "Tom's phone number starts with 555", "unrelated", "cross_topic" },
   { "User has two siblings", "Sarah drives a Subaru Outback", "unrelated", "cross_topic" },
   { "Tom is 55 years old", "User enjoys cooking Mexican food", "unrelated", "cross_topic" },
   { "Bob speaks Portuguese", "Alice has a gym membership", "unrelated", "cross_topic" },
   { "User lives in Denver", "Sarah is reading a book about space", "unrelated", "cross_topic" },
   { "Alice is pregnant", "Bob plays poker on Thursdays", "unrelated", "cross_topic" },
   { "User works the night shift", "Tom's daughter plays soccer", "unrelated", "cross_topic" },
   { "Sarah has a master's in economics", "User's cat is an indoor cat", "unrelated",
     "cross_topic" },
   { "Bob is afraid of spiders", "Alice works at a startup in Berlin", "unrelated", "cross_topic" },
   { "User bought a new laptop", "Tom is training for a triathlon", "unrelated", "cross_topic" },
   { "Alice's favorite food is sushi", "Bob has never been to Europe", "unrelated", "cross_topic" },
   { "User is colorblind", "Sarah moved to Chicago last month", "unrelated", "cross_topic" },
};

#define FACT_PAIR_COUNT (sizeof(FACT_PAIRS) / sizeof(FACT_PAIRS[0]))

#endif /* CONTRADICTION_PAIRS_H */
