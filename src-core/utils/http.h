#pragma once

/**
 * @file http.h
 * @brief HTTP Get/Post functions and others
 */

#include <string>

namespace satdump
{
    /**
     * @brief Timeouts of the HTTP requests, in seconds.
     *
     * A connection must be established within HTTP_CONNECT_TIMEOUT.
     * A request is then aborted if less than HTTP_STALL_MIN_BYTES_PER_SEC
     * bytes per second are received for HTTP_STALL_TIMEOUT, which covers
     * a server that accepts the connection but never answers, while
     * leaving large downloads that keep progressing alone. This also
     * means a request fails if the server takes longer than
     * HTTP_STALL_TIMEOUT to send the first byte of its response.
     * A POST request, which only carries small payloads (e.g. webhooks),
     * is also aborted after HTTP_POST_TIMEOUT in total.
     */
    constexpr long HTTP_CONNECT_TIMEOUT = 15;
    constexpr long HTTP_STALL_TIMEOUT = 30;
    constexpr long HTTP_STALL_MIN_BYTES_PER_SEC = 1;
    constexpr long HTTP_POST_TIMEOUT = 30;

    /**
     * @brief cURL helper function, writing
     * the payload into a std::string
     *
     * Provided a std::string is passed as userdata
     * for Curl, this can be used a handler to write
     * incoming data into said string.
     */
    size_t curl_write_std_string(void *contents, size_t size, size_t nmemb, std::string *s);

    /**
     * @brief Perform a HTTP Request on the
     * provided URL and return the result as
     * a string. Redirects are followed.
     *
     * @param url_str URL to use
     * @param result HTTP response, as a string. The response
     * is appended to it, also when the request fails
     * @param added_header optional additional headers
     * @param progress optional progress float pointer
     * @param http_status optional, set to the final HTTP status,
     * or 0 if no response was received or the protocol has no
     * status (e.g. file://)
     * @return 0 on success, 1 if the request failed or
     * the final HTTP status is 300 or above
     */
    int perform_http_request(std::string url, std::string &result, std::string added_header = "", float *progress = nullptr, long *http_status = nullptr);

    /**
     * @brief Whether a failed request is worth retrying,
     * given the HTTP status perform_http_request() reported.
     * That is the case of transport errors (no status),
     * 429 Too Many Requests and 5xx server errors, but not of
     * other statuses, such as 403 or 404, which a retry would
     * only get again.
     *
     * @param http_status status reported by perform_http_request()
     * @return true if the request may succeed when retried
     */
    bool http_request_should_retry(long http_status);

    /**
     * @brief Perform a HTTP Request on the
     * provided URL and return the result as
     * a string, with POST data.
     *
     * @param url_str URL to use
     * @param result HTTP response, as a string
     * @param post_req POST payload
     * @param added_header optional additional headers
     * @return 0 on success, 1 if the request failed or the server answered with a status of 300 or above
     */
    int perform_http_request_post(std::string url_str, std::string &result, std::string post_req, std::string added_header = "");
} // namespace satdump