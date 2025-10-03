#include <bits/stdc++.h>
using namespace std;

// ICPC Management System implementation per README requirements.
// Key operations: ADDTEAM, START, SUBMIT, FLUSH, FREEZE, SCROLL, QUERY_RANKING, QUERY_SUBMISSION, END
// Complexity targets mostly achieved using ordered maps/sets and priority data structures.

struct Submission {
    char problem; // 'A'..'Z'
    string status; // Accepted, Wrong_Answer, Runtime_Error, Time_Limit_Exceed
    int time; // time >= 1
};

struct ProblemState {
    // Before freeze info
    int wrong_before_accept = 0; // wrong attempts before first AC
    int first_ac_time = -1;      // time of first AC (non-frozen sense)

    // After freeze counters
    int wrong_before_freeze = 0; // wrong attempts strictly before freeze
    int submissions_after_freeze = 0; // all submits after freeze on this problem

    // For scrolling reveal, we must replay exact submissions between freeze and now.
    vector<Submission> post_freeze_submissions; // in arrival order

    bool solved_before_freeze = false; // computed at freeze time
    bool is_frozen = false;            // currently frozen due to submissions after freeze while unsolved at freeze

    // Runtime counters (accumulated across entire contest)
    int total_wrong_attempts = 0; // total wrong attempts so far

    // Helpers
    bool solved() const { return first_ac_time != -1; }
};

struct Team {
    string name;
    // Problems A.. up to M
    vector<ProblemState> problems; // size = problem_count

    // Ranking metrics (unfrozen-visible only)
    int solved_count = 0; // number of solved problems counted on current visible board
    long long penalty_sum = 0; // 20*wrong + time for solved problems
    vector<int> solve_times_sorted_desc; // sorted descending solve times for tie-breaking

    // Freeze state derived
    bool has_frozen_problem = false; // if any problem is currently frozen

    // For QUERY_SUBMISSION search
    vector<Submission> all_submissions; // all submissions of this team
};

struct VisibleBoardMetrics {
    int solved_count;
    long long penalty_sum;
    vector<int> solve_times_sorted_desc; // sorted descending
};

struct BoardLess {
    bool operator()(const Team* a, const Team* b) const {
        if (a->solved_count != b->solved_count) return a->solved_count > b->solved_count;
        if (a->penalty_sum != b->penalty_sum) return a->penalty_sum < b->penalty_sum;
        // Compare solve times vector in lexicographic on descending list (smaller max earlier)
        const auto &va = a->solve_times_sorted_desc;
        const auto &vb = b->solve_times_sorted_desc;
        size_t na = va.size(), nb = vb.size();
        size_t n = max(na, nb);
        for (size_t i = 0; i < n; ++i) {
            int ta = (i < na ? va[i] : -1); // missing treated as -1, which is smaller than any valid time
            int tb = (i < nb ? vb[i] : -1);
            if (ta != tb) return ta < tb; // smaller max time ranks higher
        }
        return a->name < b->name;
    }
};

class ICPCSystem {
  public:
    ICPCSystem() : started(false), frozen(false), duration_time(0), problem_count(0) {}

    void addTeam(const string &team_name) {
        if (started) {
            cout << "[Error]Add failed: competition has started.\n";
            return;
        }
        if (teams_by_name.count(team_name)) {
            cout << "[Error]Add failed: duplicated team name.\n";
            return;
        }
        Team* t = new Team();
        t->name = team_name;
        t->problems.assign(problem_count, ProblemState());
        teams_by_name[team_name] = unique_ptr<Team>(t);
        insertion_order.push_back(t);
        cout << "[Info]Add successfully.\n";
    }

    void start(int duration, int prob_cnt) {
        if (started) {
            cout << "[Error]Start failed: competition has started.\n";
            return;
        }
        started = true;
        duration_time = duration;
        problem_count = prob_cnt;
        // Resize existing teams' problem vectors
        for (Team* t : getAllRawTeams()) {
            t->problems.assign(problem_count, ProblemState());
        }
        // Before first flush, ranking is lexicographic by team name
        // We'll maintain board vector but only used when flushed
        cout << "[Info]Competition starts.\n";
    }

    void submit(char problem, const string &team_name, const string &status, int time) {
        // Validity guaranteed per statement
        Team* t = getTeam(team_name);
        if (!t) return; // should not happen per spec
        Submission s{problem, status, time};
        t->all_submissions.push_back(s);
        int idx = problem - 'A';
        if (idx < 0 || idx >= problem_count) return; // safe guard
        ProblemState &ps = t->problems[idx];

        // Track wrong attempts and AC over entire contest timeline
        bool is_ac = (status == "Accepted");
        bool is_wrong = (status == "Wrong_Answer" || status == "Runtime_Error" || status == "Time_Limit_Exceed");

        if (!frozen) {
            // Real-time update to per-problem counters
            if (!ps.solved()) {
                if (is_ac) {
                    ps.first_ac_time = time;
                } else if (is_wrong) {
                    ps.wrong_before_accept++;
                }
            }
        } else {
            // Frozen period
            if (!ps.solved_before_freeze) {
                // Problem participates in freeze mechanics
                ps.is_frozen = true;
                if (is_wrong) {
                    ps.submissions_after_freeze++;
                } else if (is_ac) {
                    // AC also counts to submissions_after_freeze per definition (y = number of submissions after freezing)
                    ps.submissions_after_freeze++;
                }
                ps.post_freeze_submissions.push_back(s);
            } else {
                // If solved before freeze, subsequent submissions do not freeze this problem
                if (!ps.solved()) {
                    // If somehow it gets new AC later (should already be solved_before_freeze), but keep correctness
                    if (is_ac) ps.first_ac_time = time;
                    else if (is_wrong) ps.wrong_before_accept++;
                }
            }
        }
    }

    void flush() {
        // Rebuild visible metrics from current problem states, excluding frozen problems contributions
        rebuildVisibleMetrics();
        // Update last flushed ordering snapshot for queries
        last_flushed_order = getOrderedTeamsByBoard();
        has_flushed = true;
        cout << "[Info]Flush scoreboard.\n";
    }

    void freeze() {
        if (frozen) {
            cout << "[Error]Freeze failed: scoreboard has been frozen.\n";
            return;
        }
        // Capture snapshot: for each team/problem compute pre-freeze counts and flags
        for (Team* t : getAllRawTeams()) {
            t->has_frozen_problem = false;
            for (int i = 0; i < problem_count; ++i) {
                ProblemState &ps = t->problems[i];
                ps.solved_before_freeze = ps.solved();
                ps.wrong_before_freeze = ps.wrong_before_accept; // wrong attempts before freeze
                ps.submissions_after_freeze = 0;
                ps.post_freeze_submissions.clear();
                ps.is_frozen = false;
            }
        }
        frozen = true;
        cout << "[Info]Freeze scoreboard.\n";
    }

    void scroll() {
        if (!frozen) {
            cout << "[Error]Scroll failed: scoreboard has not been frozen.\n";
            return;
        }
        // As per spec: first print prompt, then print scoreboard before scrolling (after flushing), then print each ranking change, then print final scoreboard.
        cout << "[Info]Scroll scoreboard.\n";
        // Ensure visible metrics represent the pre-scroll flushed board (flush silently)
        rebuildVisibleMetrics();
        last_flushed_order = getOrderedTeamsByBoard();
        has_flushed = true;
        printScoreboard();

        // We will repeatedly select the lowest-ranked team with frozen problems, then unfreeze its smallest-index frozen problem.
        // To know which problems are frozen for a team, check ps.is_frozen set at submission time after freeze, but ensure it's recomputed from post_freeze_submissions existence.
        for (Team* t : getAllRawTeams()) {
            bool hf = false;
            for (int i = 0; i < problem_count; ++i) {
                ProblemState &ps = t->problems[i];
                if (!ps.solved_before_freeze && !ps.post_freeze_submissions.empty()) {
                    ps.is_frozen = true;
                    hf = true;
                }
            }
            t->has_frozen_problem = hf;
        }

        // Build an ordered list based on current board to find lowest-ranked efficiently by reverse iterating
        vector<Team*> ordered = getOrderedTeamsByBoard();

        auto anyFrozenExists = [&]() {
            for (Team* t : getAllRawTeams()) if (t->has_frozen_problem) return true; return false;
        };

        while (anyFrozenExists()) {
            // pick lowest-ranked team with frozen problems => scan ordered from back
            Team* target = nullptr;
            for (auto it = ordered.rbegin(); it != ordered.rend(); ++it) {
                if ((*it)->has_frozen_problem) { target = *it; break; }
            }
            if (!target) break; // safety

            // pick the smallest problem index that is frozen
            int chosen_idx = -1;
            for (int i = 0; i < problem_count; ++i) {
                if (target->problems[i].is_frozen) { chosen_idx = i; break; }
            }
            if (chosen_idx == -1) { target->has_frozen_problem = false; continue; }

            // Unfreeze: replay submissions for that problem, updating team problem state and then recompute board
            ProblemState &ps = target->problems[chosen_idx];
            bool rankingChanged = false;

            // Before unfreeze, capture original rank position to detect changes
            auto originalOrdered = ordered; // copy

            // Replay
            if (!ps.solved_before_freeze) {
                for (const Submission &s : ps.post_freeze_submissions) {
                    bool is_ac = (s.status == "Accepted");
                    bool is_wrong = (s.status == "Wrong_Answer" || s.status == "Runtime_Error" || s.status == "Time_Limit_Exceed");
                    if (ps.first_ac_time == -1) {
                        if (is_ac) {
                            ps.first_ac_time = s.time;
                        } else if (is_wrong) {
                            ps.wrong_before_accept++;
                        }
                    }
                }
            }
            ps.is_frozen = false;
            ps.post_freeze_submissions.clear();

            // After unfreeze, update team flag whether any other frozen problems remain
            target->has_frozen_problem = false;
            for (int i = 0; i < problem_count; ++i) if (target->problems[i].is_frozen) { target->has_frozen_problem = true; break; }

            // Recompute scoreboard after this single unfreeze (spec says after each unfreeze that causes a ranking change, output one line)
            rebuildVisibleMetrics();
            // Compute new ordered
            ordered = getOrderedTeamsByBoard();

            // Detect if target moved up (ranking increased)
            int old_pos = findPosition(originalOrdered, target);
            int new_pos = findPosition(ordered, target);
            if (new_pos < old_pos) {
                // One or multiple steps up; output the team that previously occupied
                // the new position before this increase (from the original ordering)
                Team* replaced = nullptr;
                if (new_pos >= 0 && new_pos < (int)originalOrdered.size()) {
                    replaced = originalOrdered[new_pos];
                } else if (old_pos - 1 >= 0) {
                    // Fallback safety (should not happen)
                    replaced = originalOrdered[old_pos - 1];
                } else {
                    replaced = target; // degenerate
                }
                cout << target->name << ' ' << replaced->name << ' ' << target->solved_count << ' ' << target->penalty_sum << "\n";
                rankingChanged = true;
            }
            // continue loop until none remain
        }

        // Finally, output the scoreboard after scrolling
        printScoreboard();

        // End frozen state
        frozen = false;
        // Update last flushed ordering to reflect the final scoreboard after scrolling
        last_flushed_order = getOrderedTeamsByBoard();
        has_flushed = true;
        // Clear frozen markers
        for (Team* t : getAllRawTeams()) {
            t->has_frozen_problem = false;
            for (int i = 0; i < problem_count; ++i) {
                t->problems[i].is_frozen = false;
                t->problems[i].post_freeze_submissions.clear();
            }
        }
    }

    void queryRanking(const string &team_name) {
        Team* t = getTeam(team_name);
        if (!t) {
            cout << "[Error]Query ranking failed: cannot find the team.\n";
            return;
        }
        cout << "[Info]Complete query ranking.\n";
        if (frozen) {
            cout << "[Warning]Scoreboard is frozen. The ranking may be inaccurate until it were scrolled.\n";
        }
        // Find ranking per last flush (visible metrics)
        vector<Team*> ordered;
        if (has_flushed) {
            ordered = last_flushed_order;
        } else {
            // Before first flush, lexicographic by team name
            ordered = getAllRawTeams();
            sort(ordered.begin(), ordered.end(), [](Team* a, Team* b){ return a->name < b->name; });
        }
        int pos = findPosition(ordered, t);
        cout << t->name << " NOW AT RANKING " << (pos + 1) << "\n";
    }

    void querySubmission(const string &team_name, const string &problem, const string &status) {
        Team* t = getTeam(team_name);
        if (!t) {
            cout << "[Error]Query submission failed: cannot find the team.\n";
            return;
        }
        cout << "[Info]Complete query submission.\n";
        // Find last submission matching filters
        bool problemAll = (problem == "ALL");
        bool statusAll = (status == "ALL");
        for (int i = (int)t->all_submissions.size() - 1; i >= 0; --i) {
            const Submission &s = t->all_submissions[i];
            if (!problemAll && s.problem != problem[0]) continue;
            if (!statusAll && s.status != status) continue;
            cout << t->name << ' ' << s.problem << ' ' << s.status << ' ' << s.time << "\n";
            return;
        }
        cout << "Cannot find any submission.\n";
    }

    void end() {
        cout << "[Info]Competition ends.\n";
    }

    void processInput() {
        ios::sync_with_stdio(false);
        cin.tie(nullptr);

        string cmd;
        while (cin >> cmd) {
            if (cmd == "ADDTEAM") {
                string team; cin >> team; addTeam(team);
            } else if (cmd == "START") {
                string tmp; int duration, prob_cnt; 
                cin >> tmp; // DURATION
                cin >> duration; 
                cin >> tmp; // PROBLEM
                cin >> prob_cnt; 
                start(duration, prob_cnt);
            } else if (cmd == "SUBMIT") {
                string problem_name; string tmp; string team_name; string with; string status; string at; int tm;
                cin >> problem_name; // problem letter
                cin >> tmp; // BY
                cin >> team_name; 
                cin >> with; // WITH
                cin >> status; 
                cin >> at; // AT
                cin >> tm; 
                char p = problem_name[0];
                submit(p, team_name, status, tm);
            } else if (cmd == "FLUSH") {
                flush();
            } else if (cmd == "FREEZE") {
                freeze();
            } else if (cmd == "SCROLL") {
                scroll();
            } else if (cmd == "QUERY_RANKING") {
                string team; cin >> team; queryRanking(team);
            } else if (cmd == "QUERY_SUBMISSION") {
                string team; string where; string problem_eq; string problem_val; string and_kw; string status_eq; string status_val;
                cin >> team; 
                cin >> where; // WHERE
                cin >> problem_eq; // PROBLEM=...
                if (problem_eq.rfind("PROBLEM=", 0) == 0) {
                    problem_val = problem_eq.substr(8);
                }
                cin >> and_kw; // AND
                cin >> status_eq; // STATUS=...
                if (status_eq.rfind("STATUS=", 0) == 0) {
                    status_val = status_eq.substr(7);
                }
                querySubmission(team, problem_val, status_val);
            } else if (cmd == "END") {
                end();
                break;
            } else {
                // ignore unknown
            }
        }
    }

  private:
    bool started;
    bool frozen;
    int duration_time;
    int problem_count;

    bool has_flushed = false; // whether at least one flush (or scroll-internal flush) happened
    vector<Team*> last_flushed_order; // snapshot of ordering at last flush/scroll

    map<string, unique_ptr<Team>> teams_by_name; // maintain ownership
    vector<Team*> insertion_order; // track added order for pre-first-flush lexicographic baseline

    Team* getTeam(const string &name) {
        auto it = teams_by_name.find(name);
        if (it == teams_by_name.end()) return nullptr;
        return it->second.get();
    }

    vector<Team*> getAllRawTeams() {
        vector<Team*> v;
        v.reserve(teams_by_name.size());
        for (auto &kv : teams_by_name) v.push_back(kv.second.get());
        return v;
    }

    void rebuildVisibleMetrics() {
        for (Team* t : getAllRawTeams()) {
            t->solved_count = 0;
            t->penalty_sum = 0;
            t->solve_times_sorted_desc.clear();
            for (int i = 0; i < problem_count; ++i) {
                const ProblemState &ps = t->problems[i];
                // If frozen and this problem is in frozen state due to submissions after freeze while unsolved at freeze,
                // the scoreboard should show frozen counts and must not apply any solves that occurred after freeze (until unfreeze during scrolling)
                bool countedSolved = false;
                if (frozen && ps.is_frozen) {
                    // Do not count any new solves in frozen problems
                    countedSolved = false;
                } else {
                    if (ps.first_ac_time != -1) {
                        countedSolved = true;
                        t->solved_count += 1;
                        int wrong = ps.wrong_before_accept;
                        long long pen = 20LL * wrong + ps.first_ac_time;
                        t->penalty_sum += pen;
                        t->solve_times_sorted_desc.push_back(ps.first_ac_time);
                    }
                }
                (void)countedSolved;
            }
            sort(t->solve_times_sorted_desc.begin(), t->solve_times_sorted_desc.end(), greater<int>());
        }
    }

    vector<Team*> getOrderedTeamsByBoard(bool lexicographic_if_never_flushed = false) {
        vector<Team*> v = getAllRawTeams();
        // Before first flush, ranking is lexicographic by team name
        if (lexicographic_if_never_flushed && !started) {
            sort(v.begin(), v.end(), [](Team* a, Team* b){ return a->name < b->name; });
            return v;
        }
        sort(v.begin(), v.end(), BoardLess());
        return v;
    }

    void printScoreboard() {
        vector<Team*> ordered = getOrderedTeamsByBoard();
        for (size_t r = 0; r < ordered.size(); ++r) {
            Team* t = ordered[r];
            cout << t->name << ' ' << (r + 1) << ' ' << t->solved_count << ' ' << t->penalty_sum;
            for (int i = 0; i < problem_count; ++i) {
                const ProblemState &ps = t->problems[i];
                string cell;
                if (frozen && ps.is_frozen && !ps.solved_before_freeze) {
                    int x = ps.wrong_before_freeze;
                    int y = ps.submissions_after_freeze;
                    if (x == 0) {
                        cell = (y == 0 ? "." : ("0/" + to_string(y)));
                    } else {
                        cell = "-" + to_string(x) + "/" + to_string(y);
                    }
                } else if (ps.first_ac_time != -1) {
                    int x = ps.wrong_before_accept;
                    if (x == 0) cell = "+"; else cell = "+" + to_string(x);
                } else {
                    int x = ps.wrong_before_accept;
                    if (x == 0) cell = "."; else cell = "-" + to_string(x);
                }
                cout << ' ' << cell;
            }
            cout << "\n";
        }
    }

    int findPosition(const vector<Team*> &vec, Team* t) {
        for (int i = 0; i < (int)vec.size(); ++i) if (vec[i] == t) return i; return (int)vec.size();
    }
};

int main() {
    ICPCSystem sys;
    sys.processInput();
    return 0;
}
